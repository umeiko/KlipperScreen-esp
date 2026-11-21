/*
 * BSP: Guition ESP32-4848S040（esp32s3-4848s040，
 *   4.0" 480x480 ST7701S RGB 并口 + GT911 电容触摸，
 *   覆盖裸板 ESP32-4848S040C_I 及带继电器变体 _I_Y_1 / _I_Y_3）
 * 硬件资料来源（四份上游实机验证配置交叉核对一致）：
 *   - Espressif ESP32_Display_Panel:
 *     src/board/supported/jingcai/BOARD_JINGCAI_ESP32_4848S040C_I_Y_3.h
 *   - ESPHome devices:
 *     src/docs/devices/Guition-ESP32-S3-4848S040/index.md
 *   - openHASP:
 *     user_setups/esp32s3/esp32-s3-4848S040.ini & st7701_4848S040_init_operations
 *   - platformio-espressif32-sunton:
 *     esp32-4848S040CIY3.json
 * 硬件要点：
 *   - ESP32-S3-WROOM-1-N16R8（16MB QIO Flash + 8MB Octal PSRAM @ 80MHz）
 *   - ST7701S 480x480：无硬件复位脚（初始化先发 SWRESET），3 线 9-bit SPI 位 bang 初始化
 *     （CS=39, SCK=48, MOSI=47，SPI mode 0）；0xCD=0x00（关闭 MDT）、0x36=0x00（RGB）、
 *     0x3A=0x60（RGB666）、0x20（INVOFF）；RGB 刷新走自研 rgb44 DIRECT 双缓冲 @ 12MHz
 *   - 触摸 GT911：I2C0（SDA=19, SCL=45），无 RST/INT（轮询），自动探测 0x5D / 0x14
 *   - 背光：GPIO38 LEDC PWM 1kHz 高电平点亮（对齐 Espressif 官方板级配置，避开 100-150Hz 频闪与 5kHz 低占空比熄灭）
 *   - 无息屏按键：GPIO0（BOOT）硬连到 LCD R4，严禁注册为输入按键
 *   - 串口控制台走 CH340 的 UART0（GPIO43/44；GPIO19/20 已分配给触摸 SDA 与 LCD G1）
 *   - 板载 TF 卡槽（GPIO41/42/47/48）与继电器/I2S 音频（GPIO1/2/40）本固件未使用
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_4848S040

#include "bsp.h"
#include "bsp_screen_power.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rgb44.h"

/* ---------- 引脚定义（Guition ESP32-4848S040C_I） ---------- */
/* RGB LCD：16bit RGB565 小端，数据序 B0..B4, G0..G5, R0..R4 */
#define PIN_LCD_HSYNC  16
#define PIN_LCD_VSYNC  17
#define PIN_LCD_DE     18
#define PIN_LCD_PCLK   21
#define PIN_LCD_BL     38    /* LEDC PWM 1kHz，高电平点亮 */
#define LCD_DATA_PINS  { 4, 5, 6, 7, 15,         /* B0..B4 */ \
                         8, 20, 3, 46, 9, 10,    /* G0..G5 */ \
                         11, 12, 13, 14, 0 }     /* R0..R4（GPIO0 与 BOOT 复用） */

/* ST7701S 初始化 3 线 9-bit SPI（位 bang，无硬件 RST） */
#define PIN_SPI_CS     39
#define PIN_SPI_SCK    48
#define PIN_SPI_MOSI   47

/* 触摸 GT911：I2C0，无 RST/INT（轮询） */
#define PIN_TP_SDA     19
#define PIN_TP_SCL     45

#define LCD_H_RES      480
#define LCD_V_RES      480
#define LCD_PCLK_HZ    (12 * 1000 * 1000)   /* ~42.3fps，同 SenseCAP Indicator，PSRAM 带宽余量充足 */

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static rgb44_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;
static uint16_t *fb0, *fb1; /* 双帧缓冲（PSRAM 各 450KB），rgb44 4.4 传输模型 +
                               LVGL DIRECT 直渲 + vsync 换页，同 JC8048 / SenseCAP 方案 */

static void fb_sync(void *ptr, size_t len)
{
    esp_cache_msync(ptr, len,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* 开机动画推屏：两块 fb 都写并回写 cache（此时 LVGL 未启动，物理扫描固定在 fb0） */
static void fb_push(int x, int y, int w, int h, const uint16_t *px)
{
    for (int r = 0; r < h; r++) {
        memcpy(&fb0[(y + r) * LCD_H_RES + x], &px[r * w], (size_t)w * 2);
        memcpy(&fb1[(y + r) * LCD_H_RES + x], &px[r * w], (size_t)w * 2);
    }
    size_t row_bytes = (size_t)LCD_H_RES * (size_t)h * 2;
    fb_sync(&fb0[y * LCD_H_RES], row_bytes);
    fb_sync(&fb1[y * LCD_H_RES], row_bytes);
}

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    fb_push(x, y, w, h, px);
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* 反色 / 180° 旋转 / 水平镜像：RGB 并口屏运行时改寄存器易与扫描竞态，同 JC8048 / SenseCAP 一律不做 */
bool bsp_disp_can_invert(void)    { return false; }
bool bsp_disp_can_rotate180(void) { return false; }
bool bsp_disp_can_mirror_x(void)  { return false; }
void bsp_disp_set_invert(bool en)    { LV_UNUSED(en); }
void bsp_disp_set_rotate180(bool en) { LV_UNUSED(en); }
void bsp_disp_set_mirror_x(bool en)  { LV_UNUSED(en); }

/* 背光：LEDC PWM（GPIO38，1kHz 8bit，高电平点亮） */
static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(pct * 255 / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static uint64_t screen_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* LEDC 硬件渐变到灭（阻塞至完成）。语言切换重启前调用，避免生硬跳变 */
void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) {
        ledc_fade_func_install(0);
        fade_installed = true;
    }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);

    /* 渐暗后整屏推黑并回写 cache：否则帧缓冲残留旧画面，下次上电瞬间会闪一下 */
    size_t fb_bytes = (size_t)LCD_H_RES * LCD_V_RES * 2;
    memset(fb0, 0, fb_bytes);
    memset(fb1, 0, fb_bytes);
    fb_sync(fb0, fb_bytes);
    fb_sync(fb1, fb_bytes);
}

/* ---------- ST7701S 初始化：3 线 9-bit SPI 位 bang（SPI Mode 0） ----------
   每帧 9 位：bit8 = DC（0=命令，1=数据）+ bit7..0 = 字节，MSB 先发，
   SCK 空闲低、上升沿采样。协议与 Arduino_SWSPI / esp_lcd_panel_io_3wire_spi 一致。 */
static void st_send9(uint16_t word)
{
    for (int i = 8; i >= 0; i--) {
        gpio_set_level(PIN_SPI_SCK, 0);
        gpio_set_level(PIN_SPI_MOSI, (word >> i) & 1);
        esp_rom_delay_us(2);
        gpio_set_level(PIN_SPI_SCK, 1);
        esp_rom_delay_us(2);
    }
    gpio_set_level(PIN_SPI_SCK, 0);
}

static void st_write_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_SPI_CS, 0);
    esp_rom_delay_us(2);
    st_send9(cmd);
    for (size_t i = 0; i < len; i++) {
        st_send9((uint16_t)data[i] | 0x0100u);
    }
    esp_rom_delay_us(2);
    gpio_set_level(PIN_SPI_CS, 1);
    esp_rom_delay_us(2);
}

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint16_t delay_ms;
    uint8_t data[16];
} st_init_cmd_t;

/* Guition ESP32-4848S040C_I 初始化序列（与 Espressif BOARD_JINGCAI_ESP32_4848S040C_I_Y_3.h、
   ESPHome Guition-ESP32-S3-4848S040、openHASP st7701_4848S040 逐字节核对一致） */
static const st_init_cmd_t st7701s_init_cmds[] = {
    /* 先回 Command1 并做软复位（本板无硬件 LCD_RST，保证热重启后状态干净） */
    { 0xFF,  5,   0, { 0x77, 0x01, 0x00, 0x00, 0x00 } },
    { 0x01,  0, 120, { 0 } },

    /* Command2 BK0 */
    { 0xFF,  5,   0, { 0x77, 0x01, 0x00, 0x00, 0x10 } },
    { 0xC0,  2,   0, { 0x3B, 0x00 } },   /* 480 lines */
    { 0xC1,  2,   0, { 0x0D, 0x02 } },   /* Porch control */
    { 0xC2,  2,   0, { 0x31, 0x05 } },   /* Inversion & frame rate */
    { 0xCD,  1,   0, { 0x00 } },         /* MDT=0（16-bit RGB565 必须关 MDT，区别于 SenseCAP 的 0x08） */
    { 0xB0, 16,   0, { 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                       0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18 } },
    { 0xB1, 16,   0, { 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                       0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18 } },

    /* Command2 BK1 */
    { 0xFF,  5,   0, { 0x77, 0x01, 0x00, 0x00, 0x11 } },
    { 0xB0,  1,   0, { 0x60 } },   /* Vop = 4.7375V */
    { 0xB1,  1,   0, { 0x32 } },   /* VCOM */
    { 0xB2,  1,   0, { 0x07 } },   /* VGH = 15V */
    { 0xB3,  1,   0, { 0x80 } },
    { 0xB5,  1,   0, { 0x49 } },   /* VGL = -10.17V */
    { 0xB7,  1,   0, { 0x85 } },
    { 0xB8,  1,   0, { 0x21 } },   /* AVDD=6.6 & AVCL=-4.6 */
    { 0xC1,  1,   0, { 0x78 } },
    { 0xC2,  1,   0, { 0x78 } },
    { 0xE0,  3,   0, { 0x00, 0x1B, 0x02 } },
    { 0xE1, 11,   0, { 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                       0x00, 0x44, 0x44 } },
    { 0xE2, 12,   0, { 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                       0xEC, 0xA0, 0x00, 0x00 } },
    { 0xE3,  4,   0, { 0x00, 0x00, 0x11, 0x11 } },
    { 0xE4,  2,   0, { 0x44, 0x44 } },
    { 0xE5, 16,   0, { 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                       0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0 } },
    { 0xE6,  4,   0, { 0x00, 0x00, 0x11, 0x11 } },
    { 0xE7,  2,   0, { 0x44, 0x44 } },
    { 0xE8, 16,   0, { 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                       0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0 } },
    { 0xEB,  7,   0, { 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40 } },
    { 0xEC,  2,   0, { 0x3C, 0x00 } },
    { 0xED, 16,   0, { 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA } },

    /* Command2 BK3 (VAP & VAN) */
    { 0xFF,  5,   0, { 0x77, 0x01, 0x00, 0x00, 0x13 } },
    { 0xE5,  1,   0, { 0xE4 } },

    /* Command1：色序 RGB、接口像素格式 RGB666（0x60）、关闭反色（0x20） */
    { 0xFF,  5,   0, { 0x77, 0x01, 0x00, 0x00, 0x00 } },
    { 0x36,  1,   0, { 0x00 } },   /* MADCTL: RGB，正常扫描 */
    { 0x3A,  1,   0, { 0x60 } },   /* COLMOD: RGB666（16-bit 线序下官方设定值 0x60） */
    { 0x20,  0,   0, { 0 } },      /* Display Inversion Off（与 Espressif / ESPHome / openHASP 一致） */
    { 0x11,  0, 120, { 0 } },      /* Sleep Out */
    { 0x29,  0,  20, { 0 } },      /* Display On */
};

static void st7701s_init(void)
{
    gpio_config_t spi_io = {
        .pin_bit_mask = (1ULL << PIN_SPI_CS) | (1ULL << PIN_SPI_SCK) | (1ULL << PIN_SPI_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&spi_io));
    gpio_set_level(PIN_SPI_CS, 1);
    gpio_set_level(PIN_SPI_SCK, 0);
    gpio_set_level(PIN_SPI_MOSI, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    for (size_t i = 0; i < sizeof(st7701s_init_cmds) / sizeof(st7701s_init_cmds[0]); i++) {
        st_write_cmd(st7701s_init_cmds[i].cmd, st7701s_init_cmds[i].data, st7701s_init_cmds[i].len);
        if (st7701s_init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(st7701s_init_cmds[i].delay_ms));
        }
    }
}

/* ---------- LVGL 对接 ----------
   DIRECT 双缓冲直渲，flush_cb 三条铁律（docs/jc8048w550-rgb-display-guide.md）：
   1. 只在最后一次 flush 请求换页；
   2. 换页前 esp_cache_msync 回写脏行（GDMA 读 PSRAM 不过 cache）；
   3. 阻塞等换页在 vsync 真正生效再 flush_ready。 */
static uint32_t swap_n, swap_total_us, swap_max_us;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    if (lv_display_flush_is_last(disp)) {
        fb_sync(px_map, (size_t)LCD_H_RES * LCD_V_RES * 2);
        if (rgb44_show_fb(panel_handle, px_map)) {
            int64_t t0 = esp_timer_get_time();
            rgb44_wait_swap(panel_handle, 100);
            uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
            swap_n++;
            swap_total_us += dt;
            if (dt > swap_max_us) swap_max_us = dt;
        }
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static bool wake_swallow;               /* 息屏唤醒的那次按下：吞掉防误触 */
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    if (!touch_handle) {                    /* 触摸初始化失败：永远上报释放 */
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (esp_lcd_touch_read_data(touch_handle) == ESP_OK &&
        esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        if (bsp_screen_activity()) wake_swallow = true; /* 息屏时的按下只为唤醒 */
        if (wake_swallow) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = LV_CLAMP(0, pt[0].x, LCD_H_RES - 1);
        data->point.y = LV_CLAMP(0, pt[0].y, LCD_V_RES - 1);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wake_swallow = false;
    }
}

/* 渲染耗时统计（排障用），语义同 JC8048 / SenseCAP */
static uint32_t render_calls, render_total_us, render_max_us;
static uint32_t work_total_us, work_max_us;

static void lvgl_task(void *arg)
{
    (void)arg;
    uint32_t prev_swap_total = 0;
    for (;;) {
        bsp_lvgl_lock();
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        bsp_screen_power_poll();
        bsp_lvgl_unlock();
        uint32_t sw = swap_total_us - prev_swap_total;
        prev_swap_total = swap_total_us;
        if (sw > dt) sw = dt;
        uint32_t work = dt - sw;
        render_calls++;
        render_total_us += dt;
        if (dt > render_max_us) render_max_us = dt;
        work_total_us += work;
        if (work > work_max_us) work_max_us = work;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

void bsp_init(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();

    /* NVS（WiFi 模块会用，重复 init 安全） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* LittleFS：挂载 storage 分区到 /littlefs（配置文件），首次启动自动格式化 */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光：LEDC PWM（GPIO38，1kHz 高电平点亮）；先保持暗态，等面板与帧缓冲就绪再亮 */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    /* RGB LCD（ST7701S 480x480）：自研 rgb44 驱动（同 JC8048 / SenseCAP 方案），
       双 fb PSRAM + vsync 换页 */
    rgb44_config_t rgb_cfg = {
        .timing = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 50,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 20,
            .vsync_front_porch = 10,
            .flags = { .pclk_active_neg = 0 },
        },
        .data_gpio_nums = LCD_DATA_PINS,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .de_gpio_num = PIN_LCD_DE,
        .disp_gpio_num = GPIO_NUM_NC,
        .fb_in_psram = true,
        .double_fb = true,
    };
    ESP_ERROR_CHECK(rgb44_new(&rgb_cfg, &panel_handle));
    fb0 = rgb44_fb(panel_handle, 0);
    fb1 = rgb44_fb(panel_handle, 1);
    size_t fb_bytes = (size_t)LCD_H_RES * LCD_V_RES * 2;
    memset(fb0, 0, fb_bytes);
    memset(fb1, 0, fb_bytes);
    fb_sync(fb0, fb_bytes);
    fb_sync(fb1, fb_bytes);

    /* ST7701S 面板初始化（位 bang 3 线 9-bit SPI，CS=39/SCK=48/MOSI=47） */
    st7701s_init();
    backlight_apply(100);

    /* 触摸 GT911（I2C0，SDA=19, SCL=45，无 RST/INT）：
       先扫总线打印在线地址（排障利器），优先用 0x5D、回退 0x14；
       初始化失败不致命，避免重启循环掩盖 LCD 状态 */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    ESP_LOGI(TAG, "I2C0 scan:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(i2c_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  - 0x%02x", a);
        }
    }

    uint8_t tp_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        tp_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    } else if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        tp_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGW(TAG, "GT911 not found at 0x%02x or 0x%02x, trying default 0x%02x",
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                 ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, tp_addr);
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.dev_addr = tp_addr;
    esp_err_t tp_err = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io);
    if (tp_err == ESP_OK) {
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = LCD_H_RES,
            .y_max = LCD_V_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = { .reset = 0, .interrupt = 0 },
            .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        };
        tp_err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &touch_handle);
    }
    if (tp_err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init @ 0x%02x failed (%s), continue without touch",
                 tp_addr, esp_err_to_name(tp_err));
        touch_handle = NULL;
    } else {
        ESP_LOGI(TAG, "GT911 ready @ 0x%02x", tp_addr);
    }

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* DIRECT 双缓冲：LVGL 直渲两块 PSRAM 全帧 fb，flush 只换页不拷贝 */
    lv_display_set_buffers(disp, fb0, fb1, fb_bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    /* 本板 GPIO0（BOOT）连至 LCD R4，不注册息屏按键；依赖自动息屏超时与触摸唤醒 */

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (esp32s3-4848s040, ST7701S %dx%d + GT911, rgb44 DIRECT double-fb)",
             LCD_H_RES, LCD_V_RES);
}

/* CLI 'lcdstat'：打印并清零 vsync/dma_late/渲染耗时统计（排障用） */
void bsp_lcd_stats_print(void)
{
    uint32_t vs = 0, mn = 0, mx = 0, late = 0;
    rgb44_stats(panel_handle, &vs, &mn, &mx, &late);
    printf("lcd: vsync=%lu period=%lu..%lu us, dma_late=%lu\n"
           "  handler: n=%lu avg=%lu max=%lu us | work: avg=%lu max=%lu us | swap: n=%lu avg=%lu max=%lu us (counters reset)\n",
           (unsigned long)vs, (unsigned long)mn, (unsigned long)mx, (unsigned long)late,
           (unsigned long)render_calls,
           (unsigned long)(render_calls ? render_total_us / render_calls : 0),
           (unsigned long)render_max_us,
           (unsigned long)(render_calls ? work_total_us / render_calls : 0),
           (unsigned long)work_max_us,
           (unsigned long)swap_n,
           (unsigned long)(swap_n ? swap_total_us / swap_n : 0),
           (unsigned long)swap_max_us);
    render_calls = 0; render_total_us = 0; render_max_us = 0;
    work_total_us = 0; work_max_us = 0;
    swap_n = 0; swap_total_us = 0; swap_max_us = 0;
}

void bsp_restart(void)
{
    /* 先把「重启中」toast 画出来再重启 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_ESP32S3_4848S040 */
