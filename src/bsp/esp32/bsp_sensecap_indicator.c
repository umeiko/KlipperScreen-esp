/*
 * BSP: Seeed SenseCAP Indicator（esp32s3-sensecap-indicator，
 *   4" 480x480 ST7701S RGB 并口 + FT5x06 电容触摸，D1/D1S/D1L/D1Pro 通用）
 * 硬件资料来源：
 *   - Seeed 官方 IDF SDK（.reff/sensecap_indicator_esp32，
 *     github.com/Seeed-Solution/sensecap_indicator_esp32）——引脚/时序/初始化序列
 *   - ESPHome 设备页（devices.esphome.io/devices/Seeed-SenseCAP）——交叉验证
 * 与 JC8048W550 的主要差异：
 *   - 面板 480x480 方形，ST7701S 需 3 线 SPI（位bang）初始化：
 *     SCK/MOSI 是真 GPIO，CS/RST 挂在 TCA9535 IO 扩展器上（同 JLC-SZP 的思路）
 *   - 触摸 FT5x06（I2C0 与扩展器共总线），TP_RST 也在扩展器上（建驱动前手动复位）
 *   - 8MB flash（其它 S3 板是 16MB），专用分区表
 *   - 另有一颗 RP2040 协处理器（管传感器/蜂鸣器/SD 卡），与本固件无关，
 *     只把它的复位脚（扩展器 P8）拉高放它跑出厂固件
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_SENSECAP_INDICATOR

#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rgb44.h"

/* ---------- 引脚定义（Seeed 官方 SDK） ---------- */
/* RGB LCD：16bit 565，数据序 B0..B4,G0..G5,R0..R4 */
#define PIN_LCD_HSYNC  16
#define PIN_LCD_VSYNC  17
#define PIN_LCD_DE     18
#define PIN_LCD_PCLK   21
#define PIN_LCD_BL     45    /* LEDC PWM，高电平点亮（注意：strapping 脚，官方也这么用） */
#define LCD_DATA_PINS  { 15, 14, 13, 12, 11,   /* B0..B4 */ \
                         10, 9, 8, 7, 6, 5,    /* G0..G5 */ \
                         4, 3, 2, 1, 0 }       /* R0..R4 */

/* ST7701S 初始化 3 线 SPI（位bang）：SCK/MOSI 真 GPIO，CS/RST 在扩展器上 */
#define PIN_SPI_SCK    41
#define PIN_SPI_MOSI   48

/* I2C0：TCA9535 IO 扩展器 + FT5x06 触摸共总线 */
#define PIN_I2C_SDA    39
#define PIN_I2C_SCL    40

/* TCA9535 扩展器引脚（0..15） */
#define EXP_PIN_LCD_CS    4
#define EXP_PIN_LCD_RST   5
#define EXP_PIN_TP_RST    7
#define EXP_PIN_RP2040_RST 8   /* 拉高释放 RP2040 复位（它跑出厂固件，与本固件无关） */

#define PIN_BTN_USER   38   /* 侧键：短按息屏/唤醒（低电平有效，内部上拉） */

#define LCD_H_RES      480
#define LCD_V_RES      480
#define LCD_PCLK_HZ    (12 * 1000 * 1000)   /* 官方例程 gfx->begin(12000000L)，~42fps */

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static rgb44_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;
static uint16_t *fb0, *fb1; /* 双帧缓冲（PSRAM 各 450KB），rgb44 4.4 传输模型 +
                               LVGL DIRECT 直渲 + vsync 换页，同 JC8048 方案 */

/* 开机动画推屏：两块 fb 都写（此时 LVGL 未启动，物理扫描固定在 fb0） */
static void fb_push(int x, int y, int w, int h, const uint16_t *px)
{
    for (int r = 0; r < h; r++) {
        memcpy(&fb0[(y + r) * LCD_H_RES + x], &px[r * w], w * 2);
        memcpy(&fb1[(y + r) * LCD_H_RES + x], &px[r * w], w * 2);
    }
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

/* 反色 / 180° 旋转 / 水平镜像：ST7701S 有命令接口（位bang）理论上可做，
   但 RGB 屏运行时改 MADCTL 会与扫描竞态；同 JC8048 一律不做 */
bool bsp_disp_can_invert(void)    { return false; }
bool bsp_disp_can_rotate180(void) { return false; }
bool bsp_disp_can_mirror_x(void)  { return false; }
void bsp_disp_set_invert(bool en)    { LV_UNUSED(en); }
void bsp_disp_set_rotate180(bool en) { LV_UNUSED(en); }
void bsp_disp_set_mirror_x(bool en)  { LV_UNUSED(en); }

/* 背光：LEDC PWM（GPIO45），线性占空比（本板无 JC8048 那种非线性曲线报告） */
static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 255 / 100);
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

    /* 渐暗后整屏推黑：否则帧缓冲残留旧画面，下次上电瞬间会闪一下 */
    memset(fb0, 0, LCD_H_RES * LCD_V_RES * 2);
    memset(fb1, 0, LCD_H_RES * LCD_V_RES * 2);
}

/* ---------- TCA9535（PCA9535 兼容）IO 扩展器 ----------
   寄存器（每 8 位口一对）：0/1=输入，2/3=输出，4/5=极性，6/7=方向(1=输入)。 */
static i2c_master_dev_handle_t exp_dev;

static esp_err_t exp_write_reg(uint8_t reg_pair, uint16_t val)
{
    uint8_t buf[3] = { reg_pair, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return i2c_master_transmit(exp_dev, buf, sizeof(buf), 100);
}

static esp_err_t exp_read_reg(uint8_t reg_pair, uint16_t *val)
{
    uint8_t buf[2];
    esp_err_t err = i2c_master_transmit_receive(exp_dev, &reg_pair, 1, buf, 2, 100);
    *val = (uint16_t)(buf[0] | (buf[1] << 8));
    return err;
}

/* 读-改-写输出/方向寄存器对（pin 0..15） */
static void exp_rmw(uint8_t reg_pair, int pin, int bit_val)
{
    uint16_t v;
    if (exp_read_reg(reg_pair, &v) != ESP_OK) return;
    if (bit_val) v |= (uint16_t)(1u << pin);
    else         v &= (uint16_t)~(1u << pin);
    exp_write_reg(reg_pair, v);
}

static void exp_set_direction(int pin, int is_output) { exp_rmw(6, pin, !is_output); }
static void exp_set_level(int pin, int level)         { exp_rmw(2, pin, level); }

/* ---------- ST7701S 初始化：3 线 9-bit SPI 位bang（照抄官方 SDK 协议） ----------
   每帧 9 位：1 位 DC（0=命令 1=数据）+ 8 位数据，MSB 先发，SCK 空闲低、上升沿采样。
   官方 SPI_WriteComm 对 <0x100 的命令会先发一个全 0 帧再切 CS 发真命令——
   面板的怪癖协议，原样保留（实测可用的初始化序列不容改动）。 */
static void lcd_cs(int v)   { exp_set_level(EXP_PIN_LCD_CS, v); esp_rom_delay_us(10); }
static void lcd_sck(int v)  { gpio_set_level(PIN_SPI_SCK, v); }
static void lcd_sdo(int v)  { gpio_set_level(PIN_SPI_MOSI, v); }

static void st_spi_send9(unsigned short i)
{
    for (int n = 0; n < 9; n++) {
        lcd_sdo((i & 0x0100) ? 1 : 0);
        i <<= 1;
        lcd_sck(1);
        esp_rom_delay_us(10);
        lcd_sck(0);
        esp_rom_delay_us(10);
    }
}

static void st_write_comm(unsigned short c)
{
    lcd_cs(0);
    lcd_sck(0);
    esp_rom_delay_us(10);
    st_spi_send9(((c >> 8) & 0x00FF) | 0x2000);
    lcd_sck(1);
    esp_rom_delay_us(10);
    lcd_sck(0);
    lcd_cs(1);
    lcd_cs(0);
    st_spi_send9(c & 0x00FF);
    lcd_cs(1);
}

static void st_write_data(unsigned short d)
{
    lcd_cs(0);
    lcd_sck(0);
    esp_rom_delay_us(10);
    st_spi_send9((d & 0x00FF) | 0x0100);
    lcd_sck(1);
    esp_rom_delay_us(10);
    lcd_sck(0);
    esp_rom_delay_us(10);
    lcd_cs(1);
}

#define ST_DELAY(ms) vTaskDelay(pdMS_TO_TICKS(ms))

/* 官方初始化序列（lcd_panel_st7701s_init，480x480 面板实测可用），
   仅把 CS/SCK/SDO 宏替换为本 BSP 实现，命令/数据/延时一字未改 */
static void st7701s_init(void)
{
    /* 硬件复位（官方序列注释掉了，靠上电状态；补上更稳） */
    exp_set_level(EXP_PIN_LCD_RST, 0);
    ST_DELAY(10);
    exp_set_level(EXP_PIN_LCD_RST, 1);
    ST_DELAY(120);

    st_write_comm(0xFF);
    st_write_data(0x77);
    st_write_data(0x01);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x10);

    st_write_comm(0xC0);
    st_write_data(0x3B);   /* 480 lines */
    st_write_data(0x00);

    st_write_comm(0xC1);
    st_write_data(0x0D);
    st_write_data(0x02);

    st_write_comm(0xC2);
    st_write_data(0x31);
    st_write_data(0x05);

    st_write_comm(0xC7);
    st_write_data(0x04);

    st_write_comm(0xCD);
    st_write_data(0x08);

    st_write_comm(0xB0);
    st_write_data(0x00);
    st_write_data(0x11);
    st_write_data(0x18);
    st_write_data(0x0E);
    st_write_data(0x11);
    st_write_data(0x06);
    st_write_data(0x07);
    st_write_data(0x08);
    st_write_data(0x07);
    st_write_data(0x22);
    st_write_data(0x04);
    st_write_data(0x12);
    st_write_data(0x0F);
    st_write_data(0xAA);
    st_write_data(0x31);
    st_write_data(0x18);

    st_write_comm(0xB1);
    st_write_data(0x00);
    st_write_data(0x11);
    st_write_data(0x19);
    st_write_data(0x0E);
    st_write_data(0x12);
    st_write_data(0x07);
    st_write_data(0x08);
    st_write_data(0x08);
    st_write_data(0x08);
    st_write_data(0x22);
    st_write_data(0x04);
    st_write_data(0x11);
    st_write_data(0x11);
    st_write_data(0xA9);
    st_write_data(0x32);
    st_write_data(0x18);

    st_write_comm(0xFF);
    st_write_data(0x77);
    st_write_data(0x01);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x11);

    st_write_comm(0xB0);
    st_write_data(0x60);

    st_write_comm(0xB1);
    st_write_data(0x32);

    st_write_comm(0xB2);
    st_write_data(0x07);

    st_write_comm(0xB3);
    st_write_data(0x80);

    st_write_comm(0xB5);
    st_write_data(0x49);

    st_write_comm(0xB7);
    st_write_data(0x85);

    st_write_comm(0xB8);
    st_write_data(0x21);

    st_write_comm(0xC1);
    st_write_data(0x78);

    st_write_comm(0xC2);
    st_write_data(0x78);

    ST_DELAY(20);

    st_write_comm(0xE0);
    st_write_data(0x00);
    st_write_data(0x1B);
    st_write_data(0x02);

    st_write_comm(0xE1);
    st_write_data(0x08);
    st_write_data(0xA0);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x07);
    st_write_data(0xA0);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x44);
    st_write_data(0x44);

    st_write_comm(0xE2);
    st_write_data(0x11);
    st_write_data(0x11);
    st_write_data(0x44);
    st_write_data(0x44);
    st_write_data(0xED);
    st_write_data(0xA0);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0xEC);
    st_write_data(0xA0);
    st_write_data(0x00);
    st_write_data(0x00);

    st_write_comm(0xE3);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x11);
    st_write_data(0x11);

    st_write_comm(0xE4);
    st_write_data(0x44);
    st_write_data(0x44);

    st_write_comm(0xE5);
    st_write_data(0x0A);
    st_write_data(0xE9);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x0C);
    st_write_data(0xEB);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x0E);
    st_write_data(0xED);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x10);
    st_write_data(0xEF);
    st_write_data(0xD8);
    st_write_data(0xA0);

    st_write_comm(0xE6);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x11);
    st_write_data(0x11);

    st_write_comm(0xE7);
    st_write_data(0x44);
    st_write_data(0x44);

    st_write_comm(0xE8);
    st_write_data(0x09);
    st_write_data(0xE8);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x0B);
    st_write_data(0xEA);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x0D);
    st_write_data(0xEC);
    st_write_data(0xD8);
    st_write_data(0xA0);
    st_write_data(0x0F);
    st_write_data(0xEE);
    st_write_data(0xD8);
    st_write_data(0xA0);

    st_write_comm(0xEB);
    st_write_data(0x02);
    st_write_data(0x00);
    st_write_data(0xE4);
    st_write_data(0xE4);
    st_write_data(0x88);
    st_write_data(0x00);
    st_write_data(0x40);

    st_write_comm(0xEC);
    st_write_data(0x3C);
    st_write_data(0x00);

    st_write_comm(0xED);
    st_write_data(0xAB);
    st_write_data(0x89);
    st_write_data(0x76);
    st_write_data(0x54);
    st_write_data(0x02);
    st_write_data(0xFF);
    st_write_data(0xFF);
    st_write_data(0xFF);
    st_write_data(0xFF);
    st_write_data(0xFF);
    st_write_data(0xFF);
    st_write_data(0x20);
    st_write_data(0x45);
    st_write_data(0x67);
    st_write_data(0x98);
    st_write_data(0xBA);

    st_write_comm(0x36);
    st_write_data(0x10);

    st_write_comm(0xFF);
    st_write_data(0x77);
    st_write_data(0x01);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x13);

    st_write_comm(0xE5);
    st_write_data(0xE4);

    st_write_comm(0xFF);
    st_write_data(0x77);
    st_write_data(0x01);
    st_write_data(0x00);
    st_write_data(0x00);
    st_write_data(0x00);

    st_write_comm(0x3A);   /* 0x70 RGB888, 0x60 RGB666, 0x50 RGB565（官方值 0x60） */
    st_write_data(0x60);

    st_write_comm(0x21);   /* Display Inversion On */

    st_write_comm(0x11);   /* Sleep Out */
    ST_DELAY(120);

    st_write_comm(0x29);   /* Display On */
    ST_DELAY(120);

    /* 收尾：CS/SCK/SDO 全部拉高 */
    lcd_cs(1);
    lcd_sck(1);
    lcd_sdo(1);
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
        esp_cache_msync(px_map, LCD_H_RES * LCD_V_RES * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
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
    static bool wake_swallow;               /* 息屏唤醒的那次按下：吞掉防误触 */
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    if (!touch_handle) {                    /* 触摸初始化失败：永远上报释放 */
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
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

/* 渲染耗时统计（排障用），语义同 JC8048 */
static uint32_t render_calls, render_total_us, render_max_us;
static uint32_t work_total_us, work_max_us;

static void lvgl_task(void *arg)
{
    uint32_t prev_swap_total = 0;
    for (;;) {
        bsp_lvgl_lock();
        int64_t t0 = esp_timer_get_time();
        lv_timer_handler();
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        bsp_screen_power_poll();
        bsp_sleep_button_poll();
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

    /* 背光：LEDC PWM（GPIO45，高电平点亮），亮度由 bsp_set_brightness 调节 */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    /* I2C0 总线（TCA9535 扩展器 + FT5x06 触摸共用） */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* TCA9535 IO 扩展器：主地址 0x20，部分批次 0x39（官方 SDK 探测逻辑）。
       先扫一遍总线打印在线设备——触摸/扩展器异常时一眼定位 */
    ESP_LOGI(TAG, "I2C0 scan:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(i2c_bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  - 0x%02x", a);
        }
    }
    uint8_t exp_addr = 0x20;
    if (i2c_master_probe(i2c_bus, exp_addr, 100) != ESP_OK) {
        exp_addr = 0x39;
        ESP_LOGW(TAG, "TCA9535 not @0x20, fallback 0x39");
    }
    i2c_device_config_t exp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = exp_addr,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &exp_cfg, &exp_dev));
    ESP_LOGI(TAG, "TCA9535 @ 0x%02x", exp_addr);

    /* 扩展器初始化：触摸复位脉冲（P7）、释放 RP2040 复位（P8）、
       LCD CS/RST 拉高（P4/P5） */
    exp_set_direction(EXP_PIN_TP_RST, 1);
    exp_set_level(EXP_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    exp_set_level(EXP_PIN_TP_RST, 1);
    exp_set_direction(EXP_PIN_RP2040_RST, 1);
    exp_set_level(EXP_PIN_RP2040_RST, 1);
    exp_set_level(EXP_PIN_LCD_CS, 1);
    exp_set_level(EXP_PIN_LCD_RST, 1);
    exp_set_direction(EXP_PIN_LCD_CS, 1);
    exp_set_direction(EXP_PIN_LCD_RST, 1);
    uint16_t exp_out, exp_dir;
    if (exp_read_reg(2, &exp_out) == ESP_OK && exp_read_reg(6, &exp_dir) == ESP_OK) {
        ESP_LOGI(TAG, "TCA9535 out=0x%04x dir=0x%04x", exp_out, exp_dir);
    } else {
        ESP_LOGE(TAG, "TCA9535 readback failed!");
    }

    /* RGB LCD（ST7701S 480x480）：自研 rgb44 驱动（同 JC8048 方案），
       双 fb PSRAM + vsync 换页。时序参数见官方 SDK/Arduino 例程 */
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
    memset(fb0, 0, LCD_H_RES * LCD_V_RES * 2);
    memset(fb1, 0, LCD_H_RES * LCD_V_RES * 2);

    /* ST7701S 面板初始化（位bang 3 线 SPI + 扩展器 CS/RST） */
    gpio_config_t spi_io = {
        .pin_bit_mask = (1ULL << PIN_SPI_SCK) | (1ULL << PIN_SPI_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&spi_io));
    gpio_set_level(PIN_SPI_SCK, 1);
    gpio_set_level(PIN_SPI_MOSI, 1);
    st7701s_init();

    /* 触摸 FT5x06（I2C0）：复位已在扩展器初始化时完成，驱动内不再复位。
       失败不致命：打印错误继续跑（无触摸），避免 ESP_ERROR_CHECK 重启循环
       掩盖 LCD 链路的真实状态（JLC-SZP 排障教训） */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    /* GX 屏批次是 FT6336U @ 0x48（官方 SDK 注释），DX 批次才是 0x38——
       两个都探，在线的优先（实机扫描只见 0x48） */
    if (i2c_master_probe(i2c_bus, 0x48, 100) == ESP_OK) {
        tp_io_cfg.dev_addr = 0x48;
    } else {
        tp_io_cfg.dev_addr = 0x38;
    }
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = true, .mirror_y = true },   /* 实机：触摸对角颠倒，XY 双镜像 */
    };
    esp_err_t tp_err = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch_handle);
    if (tp_err != ESP_OK) {
        ESP_LOGE(TAG, "FT5x06 init failed (%s), continue without touch", esp_err_to_name(tp_err));
        touch_handle = NULL;
    }

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    /* DIRECT 双缓冲：LVGL 直渲两块 PSRAM 全帧 fb，flush 只换页不拷贝 */
    lv_display_set_buffers(disp, fb0, fb1, LCD_H_RES * LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    /* 侧键（GPIO38）= 息屏/唤醒按钮（低电平有效，内部上拉） */
    const bsp_sleep_button_cfg_t sleep_btns[] = {{ PIN_BTN_USER, true }};
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns, 1));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (sensecap-indicator, ST7701S %dx%d, rgb44 DIRECT double-fb)",
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

#endif /* CONFIG_BOARD_SENSECAP_INDICATOR */
