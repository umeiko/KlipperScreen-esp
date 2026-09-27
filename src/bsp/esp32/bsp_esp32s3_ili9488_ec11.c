/*
 * BSP: esp32s3-ILI9488-480_320-xpt2046-ec11
 *   ESP32-S3-DevKitC-1 N16R8 + 480x320 ILI9488 SPI 屏 + XPT2046 电阻触摸 + EC11 旋钮。
 *   典型板子：Makerbase MKS PI-TS35 V1.0（3.5" ILI9488 SPI 裸屏 + XPT2046）。
 *
 * 引脚与 esp32s3-st7796-480_320-xpt2046-ec11 完全一致（同一底座接线）：
 *   - 屏与 XPT2046 触摸**共用**一条 SPI 总线（SCK/MOSI/MISO 共用，各自 CS）
 *   - EC11 由共享 bsp_input_init() 按 sdkconfig 创建（A=13/B=14/SW=46），本 BSP 不管
 *   - RST 可省：接 3.3V 常高或与 MCU EN 共用复位（驱动 reset() 走软件复位）
 * 息屏/唤醒按钮：板载 BOOT 键（GPIO0）+ 外挂按钮（GPIO39，低电平有效）。
 *
 * 与 st7796 板的差异（atanisoft/esp_lcd_ili9488 驱动特性决定）：
 *   - ILI9488 走 4 线 SPI 只收 18-bit RGB666（COLMOD=0x66），bits_per_pixel=18，
 *     驱动在 draw_bitmap 里把 LVGL 原生小端 RGB565 逐像素转换进内部 DMA 缓冲——
 *     **flush/lcd_push 都不得再做字节交换**（st7796 板的高字节先发交换对本板有害）
 *   - 转换缓冲全局共享且紧随其后的 DMA 异步读它：每次 draw_bitmap 后必须等
 *     on_color_trans_done 才能开始下一次转换/flush_ready（同组件官方 LVGL 例程）
 *   - 转换缓冲按 LVGL 单块 draw buffer 像素数（480*40）申请，boot_anim 分带
 *     （ZFB_H=40）与 fade_out 逐行推屏均不超过该上限
 *   - 该驱动 mirror 语义与 esp_lcd 标准相反（mirror_x=true 是清 MX 位），
 *     panel_mirror_apply() 的布尔值已按此换算，默认方向与 st7796 板对齐；
 *     个别面板方向/反色不符时用设置页的 180° 旋转 / 水平镜像 / 反色开关修正
 * 触摸无真机提取的出厂默认值：touch.json 缺失/损坏即进入两点校准流程。
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_ESP32S3_ILI9488_EC11

#include "bsp.h"
#include "bsp_lcd_color_io.h"
#include "bsp_screen_power.h"
#include "bsp_sleep_button.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---------- 引脚定义（基准：esp32s3-st7789-320_240-ec11 官方接线） ---------- */
/* LCD 与 XPT2046 触摸共用 SPI2 总线（SCLK/MOSI/MISO 共用，各自 CS） */
#define PIN_LCD_SCLK   21    /* 屏 + 触摸共用 */
#define PIN_LCD_MOSI   47    /* 屏数据 + 触摸 DIN 共用 */
#define PIN_LCD_MISO   2     /* XPT2046 DOUT 坐标回读 */
#define PIN_LCD_CS     41
#define PIN_LCD_DC     40    /* RS/A0 */
#define PIN_LCD_RST    45    /* 可省：接 3.3V 常高或共用 MCU 复位 */
#define PIN_LCD_BL     42
#define PIN_TOUCH_CS   1
#define PIN_TOUCH_IRQ  GPIO_NUM_NC   /* TOUCH_INT 悬空不接：驱动轮询，唤醒/点击/滑动不依赖中断 */
#define PIN_BTN_BOOT   0
#define PIN_BTN_SLEEP  39    /* 外挂息屏按钮 ── 按键 ── GND，低电平有效 */

#define LCD_H_RES      480   /* 横屏逻辑分辨率 */
#define LCD_V_RES      320
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define DRAW_BUF_LINES 40

/* 触摸校准十字在屏幕上的位置与间距 */
#define CAL_P1_X  10
#define CAL_P1_Y  10
#define CAL_P2_X  (LCD_H_RES - 10)
#define CAL_P2_Y  (LCD_V_RES - 10)

static const char *TAG = "bsp";

static SemaphoreHandle_t lvgl_mux;
static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_touch_handle_t touch_handle;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ---------- 触摸校准（两点线性映射，原始 ADC → 屏幕坐标，LittleFS JSON 持久化） ---------- */
/* 与 E32R35T/st7796 板同一约定：斜率带符号，镜像由校准自动吸收，驱动层不做 swap/mirror。
   本板无真机提取的出厂默认值：touch.json 缺失/损坏即进入两点校准流程。 */
typedef struct {
    float xm, xc;   /* screen_x = raw_x * xm + xc */
    float ym, yc;   /* screen_y = raw_y * ym + yc */
} touch_cal_t;

#define TOUCH_CAL_PATH       "/littlefs/touch.json"
#define TOUCH_CAL_FORCE_PATH "/littlefs/.caltouch"   /* CLI caltouch 写入的强制校准标记 */

static touch_cal_t tcal;

static bool tp_read_raw(uint16_t *x, uint16_t *y)
{
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        /* 交换后 *x 恒为屏幕水平（长）轴原始值、*y 为垂直（短）轴原始值。
           轴向/镜像最终由两点校准吸收 */
        *x = pt[0].y;
        *y = pt[0].x;
        return true;
    }
    return false;
}

static void touch_cal_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "xCalM", tcal.xm);
    cJSON_AddNumberToObject(root, "yCalM", tcal.ym);
    cJSON_AddNumberToObject(root, "xCalC", tcal.xc);
    cJSON_AddNumberToObject(root, "yCalC", tcal.yc);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;
    FILE *f = fopen(TOUCH_CAL_PATH, "w");
    if (f) {
        fputs(str, f);
        fclose(f);
        ESP_LOGI(TAG, "touch cal saved: %s", str);
    }
    free(str);
}

static bool touch_cal_load(void)
{
    /* CLI `caltouch` 留下的强制校准标记：删除后返回 false → 进两点校准 */
    FILE *ff = fopen(TOUCH_CAL_FORCE_PATH, "r");
    if (ff) {
        fclose(ff);
        remove(TOUCH_CAL_FORCE_PATH);
        ESP_LOGW(TAG, "touch calibration forced via CLI");
        return false;
    }
    char buf[160] = {0};
    FILE *f = fopen(TOUCH_CAL_PATH, "r");
    if (f) {
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *xm = cJSON_GetObjectItem(root, "xCalM");
            cJSON *ym = cJSON_GetObjectItem(root, "yCalM");
            cJSON *xc = cJSON_GetObjectItem(root, "xCalC");
            cJSON *yc = cJSON_GetObjectItem(root, "yCalC");
            if (cJSON_IsNumber(xm) && cJSON_IsNumber(ym) &&
                cJSON_IsNumber(xc) && cJSON_IsNumber(yc)) {
                tcal.xm = xm->valuedouble;
                tcal.ym = ym->valuedouble;
                tcal.xc = xc->valuedouble;
                tcal.yc = yc->valuedouble;
                ESP_LOGI(TAG, "touch cal loaded: xm=%.4f xc=%.1f ym=%.4f yc=%.1f",
                         tcal.xm, tcal.xc, tcal.ym, tcal.yc);
                cJSON_Delete(root);
                return true;
            }
            cJSON_Delete(root);
        }
        ESP_LOGW(TAG, "touch cal file invalid, entering two-point calibration");
        return false;
    }
    /* 首次启动无校准文件：无出厂默认值可写，直接进两点校准 */
    ESP_LOGW(TAG, "touch cal not found, entering two-point calibration");
    return false;
}

/* 校准时 LVGL 任务尚未启动，手动泵 lv_timer_handler */
static void cal_pump(void)
{
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* 按住采样 8 次取平均，滤掉电阻屏噪声 */
static void cal_sample(uint16_t *x, uint16_t *y)
{
    uint32_t ax = 0, ay = 0;
    int n = 0;
    while (n < 8) {
        uint16_t rx, ry;
        if (tp_read_raw(&rx, &ry)) { ax += rx; ay += ry; n++; }
        cal_pump();
    }
    *x = ax / 8;
    *y = ay / 8;
}

static void touch_cal_run(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Touch Calibration");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Press the cross");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 60);

    /* 十字线：一横一竖两个白色细矩形 */
    lv_obj_t *ch = lv_obj_create(scr);
    lv_obj_remove_style_all(ch);
    lv_obj_set_size(ch, 22, 2);
    lv_obj_set_style_bg_color(ch, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
    lv_obj_t *cv = lv_obj_create(scr);
    lv_obj_remove_style_all(cv);
    lv_obj_set_size(cv, 2, 22);
    lv_obj_set_style_bg_color(cv, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cv, LV_OPA_COVER, 0);

    lv_screen_load(scr);

    uint16_t x1, y1, x2, y2, rx, ry;

    lv_obj_set_pos(ch, CAL_P1_X - 11, CAL_P1_Y - 1);
    lv_obj_set_pos(cv, CAL_P1_X - 1, CAL_P1_Y - 11);
    while (tp_read_raw(&rx, &ry)) cal_pump();   /* 等松开 */
    while (!tp_read_raw(&rx, &ry)) cal_pump();  /* 等按下 */
    cal_sample(&x1, &y1);

    lv_obj_set_pos(ch, CAL_P2_X - 11, CAL_P2_Y - 1);
    lv_obj_set_pos(cv, CAL_P2_X - 1, CAL_P2_Y - 11);
    while (tp_read_raw(&rx, &ry)) cal_pump();
    while (!tp_read_raw(&rx, &ry)) cal_pump();
    cal_sample(&x2, &y2);

    tcal.xm = (float)(CAL_P2_X - CAL_P1_X) / ((float)x2 - (float)x1);
    tcal.xc = (float)CAL_P1_X - (float)x1 * tcal.xm;
    tcal.ym = (float)(CAL_P2_Y - CAL_P1_Y) / ((float)y2 - (float)y1);
    tcal.yc = (float)CAL_P1_Y - (float)y1 * tcal.ym;
    touch_cal_save();
    ESP_LOGI(TAG, "touch cal done: raw(%u,%u)-(%u,%u) xm=%.4f xc=%.1f ym=%.4f yc=%.1f",
             x1, y1, x2, y2, tcal.xm, tcal.xc, tcal.ym, tcal.yc);

    lv_label_set_text(hint, "Done");
    for (int i = 0; i < 50; i++) cal_pump();
}

/* ---------- 开机动画推屏（boot_anim 经 bsp.h 调用，LVGL 锁由调用方持有） ---------- */
/* esp_lcd_panel_draw_bitmap 是 DMA 异步传输：必须等 on_color_trans_done 再复用
   像素缓冲；ILI9488 驱动内部转换缓冲更是全局共享，不等完成会踩 DMA 在读的数据 */
static SemaphoreHandle_t lcd_trans_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    LV_UNUSED(io); LV_UNUSED(edata); LV_UNUSED(user_ctx);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(lcd_trans_done, &hp);
    return hp == pdTRUE;
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    /* ILI9488 驱动内部完成 RGB565→RGB666 转换，无需字节交换（与 st7796 板相反） */
    xSemaphoreTake(lcd_trans_done, 0);   /* 排掉 LVGL flush 可能留下的存量信号 */
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, px);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lcd_push draw err %s: %d,%d %dx%d", esp_err_to_name(err), x, y, w, h);
    } else if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "lcd_push wait timeout: %d,%d %dx%d", x, y, w, h);
    }
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------- 反色 / 180° 旋转 / 水平镜像（运行时生效，设置项由 app 层落盘/回读） ---------- */
static bool disp_rot180, disp_mirrorx;

bool bsp_disp_can_invert(void)    { return true; }
bool bsp_disp_can_rotate180(void) { return true; }
bool bsp_disp_can_mirror_x(void)  { return true; }

void bsp_disp_set_invert(bool en)
{
    if (panel_handle) esp_lcd_panel_invert_color(panel_handle, en);
}

/* atanisoft ILI9488 驱动的 mirror 语义与 esp_lcd 标准驱动相反：
   mirror_x=true 是**清** MX 位、mirror_x=false 是置 MX 位（MY 语义正常）。
   默认 mirror(false,true) 得 MX|MY（叠加 swap_xy 的 MV），与 st7796 板实测方向对齐。
   swap_xy 下屏幕水平轴对应面板 Y 轴：panel_mx = rot180；panel_my = rot180 ^ mirror_x 取反 */
static void panel_mirror_apply(void)
{
    if (panel_handle)
        esp_lcd_panel_mirror(panel_handle,
                             disp_rot180, disp_rot180 == disp_mirrorx);
}

void bsp_disp_set_rotate180(bool en)
{
    disp_rot180 = en;
    panel_mirror_apply();
}

void bsp_disp_set_mirror_x(bool en)
{
    disp_mirrorx = en;
    panel_mirror_apply();
}

/* 板级背光实现：本板非零占空比至少 5%，逻辑亮度与息屏状态由公共状态机管理。 */
static void backlight_apply(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0 && pct < 5) pct = 5;
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

    /* 渐暗后把 GRAM 整屏推黑：否则面板寄存器残留旧帧，下次上电瞬间会闪一下旧画面。
       不保留任何常驻缓冲：栈上现场填一行全 0，逐行推完即释放 */
    uint16_t black[LCD_H_RES];
    memset(black, 0, sizeof(black));   /* RGB565 0x0000 = 黑 */
    for (int y = 0; y < LCD_V_RES; y++) {
        xSemaphoreTake(lcd_trans_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, black);
        xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    }
}

/* ---------- LVGL 对接 ---------- */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ILI9488 驱动把 RGB565 逐像素转换进内部共享 DMA 缓冲（18-bit RGB666），
       转换与后续 DMA 都碰这块缓冲：必须等本次传输完成再给 flush_ready，
       否则下一帧渲染的转换会踩正在 DMA 的数据（组件官方 LVGL 例程同款等待） */
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "flush wait trans done timeout");
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint16_t last_rx, last_ry;       /* 最近一次有效按压的原始坐标 */
    static int64_t last_valid_us;           /* 最近一次有效按压的时间戳 */
    static bool pressing;                   /* 是否处于一次按压过程中 */
    static bool wake_swallow;               /* 息屏唤醒的那次按下：吞掉防误触 */

    uint16_t rx, ry;
    if (tp_read_raw(&rx, &ry)) {
        if (bsp_screen_activity()) wake_swallow = true; /* 息屏时的按下只为唤醒 */
        last_rx = rx;
        last_ry = ry;
        last_valid_us = esp_timer_get_time();
        pressing = true;
    } else if (pressing && esp_timer_get_time() - last_valid_us < 50 * 1000) {
        /* 滑动中压力/采样瞬时丢失时桥接为仍按住，否则手势被拆成一串点按 */
        rx = last_rx;
        ry = last_ry;
    } else {
        pressing = false;
        wake_swallow = false;                   /* 抬手，恢复交互 */
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (wake_swallow) {                         /* 唤醒点击不触发任何元素 */
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int32_t sx = (int32_t)lroundf(rx * tcal.xm + tcal.xc);
    int32_t sy = (int32_t)lroundf(ry * tcal.ym + tcal.yc);
    if (disp_rot180) {                      /* 显示翻 180° 时触摸坐标同步翻转 */
        sx = LCD_H_RES - 1 - sx;
        sy = LCD_V_RES - 1 - sy;
    }
    if (disp_mirrorx) sx = LCD_H_RES - 1 - sx;   /* 水平镜像：触摸 X 同步翻转 */
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = LV_CLAMP(0, sx, LCD_H_RES - 1);
    data->point.y = LV_CLAMP(0, sy, LCD_V_RES - 1);
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

    /* LittleFS：挂载 storage 分区到 /littlefs，存放 touch.json 触摸校准参数 */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光：LEDC PWM（GPIO42，高电平点亮），亮度由 bsp_set_brightness 调节 */
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

    /* SPI2 总线（LCD + XPT2046 触摸共用）。
       ILI9488 走 18-bit：单块 draw buffer 全量推屏是 480*40*3 字节 */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 3 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO + ILI9488 */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

    /* 传输完成信号量：flush / bsp_lcd_push 等待 DMA 完成用（保护驱动内部共享转换缓冲） */
    lcd_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_color_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

    ESP_ERROR_CHECK(bsp_lcd_color_io_wrap(&io_handle, true));
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 18,   /* 4 线 SPI 下 ILI9488 只收 18-bit RGB666（COLMOD=0x66） */
    };
    /* 转换缓冲按 LVGL 单块 draw buffer 像素数申请（驱动内部 *3 字节、DMA 属性）：
       必须 ≥ 任何一次 draw_bitmap 的像素数（flush/boot_anim 分带/fade_out 均不超） */
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9488(io_handle, &panel_cfg,
                                              LCD_H_RES * DRAW_BUF_LINES, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    /* 默认不开反色；反色是运行时开关（bsp_disp_set_invert）。
       个别 IPS 个体需要 INVON：设置页打开反色即可，无需改固件 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    /* 横屏 480x320，mirror 组合与 st7796 板实测方向对齐（布尔语义见 panel_mirror_apply） */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    panel_mirror_apply();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    /* 触摸 XPT2046：与 LCD 共用 SPI2；取原始 ADC，不做驱动层坐标换算/镜像（由两点校准吸收） */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 4096,   /* 原始 12bit ADC 空间 */
        .y_max = 4096,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = PIN_TOUCH_IRQ,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle));

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

    /* 本板无出厂校准值：touch.json 缺失/损坏即进两点校准（结果落盘后正常加载）；
       个体差异大时可用 CLI `caltouch` 强制重校 */
    if (!touch_cal_load()) {
        touch_cal_run();
    }

    /* 息屏/唤醒按钮（BOOT + GPIO39 外挂按钮，低电平有效） */
    const bsp_sleep_button_cfg_t sleep_btns[] = {
        { PIN_BTN_BOOT, true },
        { PIN_BTN_SLEEP, true },
    };
    ESP_ERROR_CHECK(bsp_sleep_button_init(sleep_btns,
                                          sizeof(sleep_btns) / sizeof(sleep_btns[0])));

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (esp32s3-ILI9488-480_320-xpt2046-ec11, %dx%d)",
             LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    /* 先把「重启中」toast 画出来再重启 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_ESP32S3_ILI9488_EC11 */
