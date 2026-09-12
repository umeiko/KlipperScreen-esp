/*
 * BSP: 立创实战派 ESP32-S3（esp32s3-JLC-SZP，2.0" 240x320 ST7789 SPI + FT6336 电容触摸）
 * 逻辑分辨率 320x240 横屏（LVGL 缓冲/flush 模式同 CYD：DMA 双缓冲 + PARTIAL）。
 *
 * 【本板LCD为什么不用 esp_lcd 面板驱动】
 * 这块屏的片选 CS 在 PCA9557（I2C 0x19）P0 上，且面板要求每笔 SPI 交易都有 CS
 * 下降沿来同步串行位计数器——CS 常低（mode 2/mode 3 均试过）屏幕全黑不工作。
 * esp_lcd 面板驱动无法经 100kHz I2C 逐笔翻转 CS，故本 BSP 直接用 SPI master +
 * 手动控 CS/DC，1:1 复刻用户实测可亮的 Arduino 工程（其 fork 的 TFT_eSPI 把
 * CS_L/CS_H 挂钩到 PCA9557 P0 翻转）：
 *   - SPI mode 3（CPOL=1/CPHA=1，ST7789 上升沿采样；教程文档的 mode 2 是下降沿，实测全黑）
 *   - 颜色顺序 BGR（TFT_RGB_ORDER=TFT_BGR），像素高字节先发（setSwapBytes(true)）
 *   - 横屏 MADCTL = MX|MV|BGR = 0x68；180° = MY|MV|BGR = 0xC8
 *   - 初始化序列照抄 TFT_eSPI ST7789_Init.h
 * 初始化时序：PCA9557 config(0x03) 写 0xFA（P0/P2 输出，P1 保持输入——对齐 Arduino
 * 工程的电气状态）→ output(0x01) 写 0xFB（P0=1 CS 空闲高；P2=0 "摄像头电源"开，
 * 该电源轨疑似与 TFT 逻辑供电共用，P2=1 时背光亮但整屏黑）→ ST7789 初始化序列。
 *
 * 背光低电平点亮：LEDC 10bit output_invert，duty 越大输出低电平越久越亮。
 * 触摸 FT6336 电容（与 PCA9557 同一 I2C0 总线），驱动按 x_max/y_max + swap/mirror
 * 直接输出屏幕坐标，无需校准。
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_JLC_SZP

#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---------- 引脚定义（立创实战派 ESP32-S3 厂商文档/wiki） ---------- */
/* LCD 走 SPI3，无 CS/RST GPIO：CS 在 PCA9557 P0（需逐笔翻转），RST 硬件常释放（NC） */
#define PIN_LCD_SCLK   41
#define PIN_LCD_MOSI   40
#define PIN_LCD_DC     39
#define PIN_LCD_BL     42    /* LEDC PWM，低电平点亮（output_invert） */

/* PCA9557（I2C0 @ 0x19）：P0=LCD_CS（低有效，逐笔翻转）、P1=PA_EN、P2=DVP_PWDN */
#define PCA9557_ADDR        0x19
#define PCA9557_REG_INPUT   0x00
#define PCA9557_REG_OUTPUT  0x01
#define PCA9557_REG_CONFIG  0x03

/* 触摸 FT6336：与 PCA9557 同一 I2C0 总线，无 RST/INT（轮询） */
#define PIN_I2C_SDA     1
#define PIN_I2C_SCL     2
#define PIN_BTN_BOOT    0   /* 板载用户键：按下息屏，再按唤醒 */

#define LCD_H_RES      320   /* 横屏逻辑分辨率（面板原生 240x320） */
#define LCD_V_RES      240
#define LCD_SPI_HZ     (80 * 1000 * 1000)
#define DRAW_BUF_LINES 40

/* ST7789 命令 */
#define ST7789_SLPOUT     0x11
#define ST7789_NORON      0x13
#define ST7789_INVOFF     0x20
#define ST7789_INVON      0x21
#define ST7789_DISPON     0x29
#define ST7789_CASET      0x2A
#define ST7789_RASET      0x2B
#define ST7789_RAMWR      0x2C
#define ST7789_MADCTL     0x36
#define ST7789_COLMOD     0x3A

/* MADCTL：横屏基准 MX|MV|BGR；180° 翻转为 MY|MV|BGR */
#define MADCTL_LAND       0x68
#define MADCTL_LAND_180   0xC8

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static spi_device_handle_t lcd_spi;
static esp_lcd_touch_handle_t touch_handle;
static i2c_master_dev_handle_t pca9557_dev;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ---------- PCA9557（LCD_CS 等扩展 IO） ---------- */
static void pca9557_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    ESP_ERROR_CHECK(i2c_master_transmit(pca9557_dev, buf, sizeof(buf), -1));
}

/* CS 翻转：空闲 0xFB（P0=1 高），选中 0xFA（P0=0 低）。
   P2=0 保持"摄像头电源"开启——用户实测可亮的 Arduino 工程全程 P2=0（camera_init 拉低），
   该电源轨疑似与 TFT 逻辑供电共用：P2=1 时背光亮但整屏黑（SPI/CS 全正常）。 */
static void lcd_cs(int level)
{
    pca9557_write(PCA9557_REG_OUTPUT, level ? 0xFB : 0xFA);
}

/* ---------- ST7789 直接 SPI 驱动（复刻 TFT_eSPI 打法） ---------- */
static void lcd_tx(const void *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

/* CS 帧内写命令字节（DC 低） */
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    lcd_tx(&cmd, 1);
}

/* CS 帧内写数据（DC 高） */
static void lcd_data(const void *data, size_t len)
{
    gpio_set_level(PIN_LCD_DC, 1);
    if (len) lcd_tx(data, len);
}

/* 独立 CS 帧：命令 + 参数（初始化用，保证每笔都有 CS 沿） */
static void lcd_cmd_params(uint8_t cmd, const uint8_t *params, size_t n)
{
    lcd_cs(0);
    lcd_cmd(cmd);
    lcd_data(params, n);
    lcd_cs(1);
}

/* 一整个 CS 帧推完一个矩形：CASET+RASET+RAMWR+像素（像素必须 DMA 内存且已字节交换） */
static void lcd_push_pixels(int x0, int y0, int x1, int y1, const void *px, size_t len)
{
    uint8_t col[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    lcd_cs(0);
    lcd_cmd(ST7789_CASET);
    lcd_data(col, 4);
    lcd_cmd(ST7789_RASET);
    lcd_data(row, 4);
    lcd_cmd(ST7789_RAMWR);
    lcd_data(px, len);
    lcd_cs(1);
}

/* ST7789 初始化序列：照抄用户实测可亮的 TFT_eSPI ST7789_Init.h（240x320 JLX240） */
static void st7789_init_sequence(void)
{
    /* 本板无 LCD RST 引脚，软件复位必须有（TFT_eSPI init 同序）：SWRESET 0x01 + 150ms */
    lcd_cmd_params(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd_params(ST7789_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd_params(ST7789_NORON, NULL, 0);

    /* display and color format setting */
    lcd_cmd_params(ST7789_MADCTL, (uint8_t[]){ 0x08 }, 1);            /* BGR，竖屏，方向最后统一设 */
    lcd_cmd_params(0xB6, (uint8_t[]){ 0x0A, 0x82 }, 2);
    lcd_cmd_params(0xB0, (uint8_t[]){ 0x00, 0xE0 }, 2);               /* RAMCTRL：5/6-bit 转换 */
    lcd_cmd_params(ST7789_COLMOD, (uint8_t[]){ 0x55 }, 1);            /* RGB565 */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Frame rate / porch */
    lcd_cmd_params(0xB2, (uint8_t[]){ 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 5);  /* PORCTRL */
    lcd_cmd_params(0xB7, (uint8_t[]){ 0x35 }, 1);                          /* GCTRL：VGH/VGL */

    /* Power setting */
    lcd_cmd_params(0xBB, (uint8_t[]){ 0x28 }, 1);                          /* VCOMS */
    lcd_cmd_params(0xC0, (uint8_t[]){ 0x0C }, 1);                          /* LCMCTRL */
    lcd_cmd_params(0xC2, (uint8_t[]){ 0x01, 0xFF }, 2);                    /* VDVVRHEN */
    lcd_cmd_params(0xC3, (uint8_t[]){ 0x10 }, 1);                          /* VRHS */
    lcd_cmd_params(0xC4, (uint8_t[]){ 0x20 }, 1);                          /* VDVSET */
    lcd_cmd_params(0xC6, (uint8_t[]){ 0x0F }, 1);                          /* FRCTR2 */
    lcd_cmd_params(0xD0, (uint8_t[]){ 0xA4, 0xA1 }, 2);                    /* PWCTRL1 */

    /* Gamma */
    lcd_cmd_params(0xE0, (uint8_t[]){ 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                                      0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17 }, 14);
    lcd_cmd_params(0xE1, (uint8_t[]){ 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                                      0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E }, 14);

    /* 本板 IPS 面板必须 INVON 才是正常颜色（与 TFT_eSPI 序列一致；INVOFF 全屏反色实测） */
    lcd_cmd_params(ST7789_INVON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd_params(ST7789_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------- 开机动画推屏（boot_anim 经 bsp.h 调用，LVGL 锁由调用方持有） ---------- */
void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    /* ST7789 走 SPI 要求先发像素高字节：拷一份交换字节再推（动画核心缓冲要复用，不能就地改） */
    size_t n = (size_t)w * h;
    uint16_t *tmp = heap_caps_malloc(n * 2, MALLOC_CAP_DMA);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    lcd_push_pixels(x, y, x + w - 1, y + h - 1, tmp, n * 2);
    free(tmp);
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------- 反色 / 180° 旋转（运行时生效，设置项由 app 层落盘/回读） ---------- */
static bool disp_rot180;

bool bsp_disp_can_invert(void)    { return true; }
bool bsp_disp_can_rotate180(void) { return true; }

void bsp_disp_set_invert(bool en)
{
    /* 本板必须 INVON 才是正常显示，故开关语义取反（同 bsp_ec11_knob_esp32 的约定） */
    lcd_cmd_params(en ? ST7789_INVOFF : ST7789_INVON, NULL, 0);
}

void bsp_disp_set_rotate180(bool en)
{
    disp_rot180 = en;
    /* 基准横屏 MADCTL 0x68（MX|MV|BGR）；180° = 0xC8（MY|MV|BGR） */
    lcd_cmd_params(ST7789_MADCTL, (uint8_t[]){ en ? MADCTL_LAND_180 : MADCTL_LAND }, 1);
}

/* 背光：GPIO42 低电平点亮，LEDC 10bit output_invert——duty 越大输出低越久越亮
   （厂商代码同式：duty = 1023 * pct / 100）。息屏逻辑由公共状态机管理。 */
static uint16_t bl_duty = 1023;

static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    bl_duty = (uint16_t)(pct * 1023 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
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
    bl_duty = 0;

    /* 渐暗后把 GRAM 整屏推黑：否则面板寄存器残留旧帧，下次上电瞬间会闪一下旧画面 */
    static uint16_t black[LCD_H_RES * DRAW_BUF_LINES];   /* 静态零初始化即全黑（RGB565 0x0000） */
    for (int y = 0; y < LCD_V_RES; y += DRAW_BUF_LINES) {
        lcd_push_pixels(0, y, LCD_H_RES - 1, y + DRAW_BUF_LINES - 1,
                        black, LCD_H_RES * DRAW_BUF_LINES * 2);
    }
}

/* ---------- LVGL 对接 ---------- */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ST7789 走 SPI 要求先发像素高字节，LVGL 内存是小端 RGB565 → 就地交换字节。
       （不用 LV_COLOR_FORMAT_RGB565_SWAPPED：该格式在本版 LVGL 渲染路径上有问题，实测雪花屏） */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    lcd_push_pixels(area->x1, area->y1, area->x2, area->y2, px_map, (size_t)n * 2);
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
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        if (bsp_screen_activity()) wake_swallow = true; /* 息屏时的按下只为唤醒 */
        if (wake_swallow) {                     /* 唤醒点击不触发任何元素 */
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->state = LV_INDEV_STATE_PRESSED;
        /* 驱动按 x_max/y_max + swap/mirror 直接输出屏幕坐标 */
        int32_t sx = pt[0].x;
        int32_t sy = pt[0].y;
        if (disp_rot180) {                  /* 显示翻 180° 时触摸坐标同步翻转 */
            sx = LCD_H_RES - 1 - sx;
            sy = LCD_V_RES - 1 - sy;
        }
        data->point.x = LV_CLAMP(0, sx, LCD_H_RES - 1);
        data->point.y = LV_CLAMP(0, sy, LCD_V_RES - 1);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        wake_swallow = false;                   /* 抬手，恢复交互 */
    }
}

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_screen_power_poll();
        bsp_sleep_button_poll();
        bsp_lvgl_unlock();
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

    /* 背光：LEDC PWM 10bit（GPIO42，低电平点亮 → output_invert），
       亮度由 bsp_set_brightness 调节 */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
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
        .duty = 1023,
        .hpoint = 0,
        .flags = { .output_invert = 1 },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
    bsp_screen_power_init(backlight_apply, screen_now_ms);

    /* I2C0 总线（PCA9557 + FT6336 触摸共用，100kHz） */
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

    /* PCA9557：对齐用户实测可亮的 Arduino 工程电气状态——
       config 0xFA：仅 P0(CS)、P2(摄像头电源)为输出，P1 保持输入（Arduino 从不配置 P1）；
       output 0xFB：P0=1（CS 空闲高）、P2=0（摄像头/TFT 电源开） */
    i2c_device_config_t pca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = 100 * 1000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &pca_cfg, &pca9557_dev));
    pca9557_write(PCA9557_REG_CONFIG, 0xFA);
    pca9557_write(PCA9557_REG_OUTPUT, 0xFB);

    /* DC 引脚 */
    gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);

    /* SPI3 总线 + LCD 设备（无 CS GPIO：CS 由我们经 PCA9557 逐笔翻转），
       spi_mode=3：ST7789 上升沿采样（教程文档 mode 2 实测全黑不工作） */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &devcfg, &lcd_spi));

    /* ST7789 初始化（复刻 TFT_eSPI 序列），随后设横屏 MADCTL */
    st7789_init_sequence();
    lcd_cmd_params(ST7789_MADCTL, (uint8_t[]){ MADCTL_LAND }, 1);

    /* 触摸 FT6336（I2C0）：驱动按 x_max/y_max + swap/mirror 直接输出屏幕坐标，无需校准 */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_V_RES,     /* 面板原生 240x320，swap_xy 后输出横屏坐标 */
        .y_max = LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = true, .mirror_x = true, .mirror_y = false },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch_handle));

    /* LVGL */
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t buf_sz = LCD_H_RES * DRAW_BUF_LINES * 2;
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    /* 板载用户键（GPIO0）= 息屏/唤醒按钮（低电平有效，内部上拉） */
    const bsp_sleep_button_cfg_t sleep_btns[] = {{ PIN_BTN_BOOT, true }};
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns, 1));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (JLC-SZP ESP32-S3, %dx%d)", LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    /* 先把「重启中」toast 画出来再重启 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_ESP32S3_JLC_SZP */
