/*
 * BSP: ESP32-32E 3.5" E32R35T（320x480 ST7796 + XPT2046 电阻触摸）
 * 逻辑分辨率 480x320 横屏。
 *
 * 与 CYD 2432S028R 的差异（据 lcdwiki 3.5inch_ESP32-32E 引脚分配表）：
 *   - 驱动 IC ILI9341 → ST7796U，分辨率 240x320 → 320x480
 *   - 触摸与液晶屏**共用** SPI2 总线（厂商设计如此，MISO 也共用）
 *   - LCD 无独立 RST 引脚（与 ESP32 EN 共用复位）→ 驱动走软件复位
 *   - 背光 GPIO27（高电平点亮）
 * 真机已验证：反色默认关（本板 ST7796 不需 INVON，开了呈底片）；
 * mirror(true,true)+swap_xy 方向正确；触摸出厂默认值已提取自真机校准，
 * 开机免校准，偏差大的个体用串口 CLI caltouch 重校。
 */
#include "sdkconfig.h"
#if CONFIG_BOARD_E32R35T

#include "bsp.h"

#include <math.h>
#include <stdio.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_st7796.h"
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

/* ---------- 引脚定义（lcdwiki 3.5inch_ESP32-32E 官方引脚分配表） ---------- */
/* LCD 与触摸 XPT2046 共用 SPI2 总线（SCLK/MOSI/MISO 共用，各自 CS） */
#define PIN_LCD_SCLK   14
#define PIN_LCD_MOSI   13
#define PIN_LCD_MISO   12
#define PIN_LCD_CS     15
#define PIN_LCD_DC     2
#define PIN_LCD_RST    (-1)   /* 与 ESP32 EN 共用复位，驱动内走软件复位 */
#define PIN_LCD_BL     27
#define PIN_TOUCH_CS   33
#define PIN_TOUCH_IRQ  36

#define LCD_H_RES      480   /* 横屏逻辑分辨率 */
#define LCD_V_RES      320
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define DRAW_BUF_LINES 40

/* 触摸校准十字在屏幕上的位置与间距（与参考实现一致） */
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
/* 参考 .reff/esp32-touchscreen-stepper-driver：斜率带符号，镜像由校准自动吸收，
   因此驱动层不做 swap/mirror，直接取 12bit 原始 ADC 值。 */
typedef struct {
    float xm, xc;   /* screen_x = raw_x * xm + xc */
    float ym, yc;   /* screen_y = raw_y * ym + yc */
} touch_cal_t;

/* 出厂默认值：从首台真机两点校准结果提取（touch.json：
   {"xCalM":0.13023783,"yCalM":0.08733624,"xCalC":-30.89468,"yCalC":-17.94760}）。
   文件缺失时直接可用；偏差大的个体仍可串口 CLI caltouch 重新校准 */
#define TOUCH_CAL_DEFAULT \
    { 0.13023783266544342f, -30.894680023193359f, 0.0873362421989441f, -17.947597503662109f }

#define TOUCH_CAL_PATH  "/littlefs/touch.json"

static touch_cal_t tcal = TOUCH_CAL_DEFAULT;

static bool tp_read_raw(uint16_t *x, uint16_t *y)
{
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(touch_handle);
    if (esp_lcd_touch_get_data(touch_handle, pt, &count, 1) == ESP_OK && count > 0) {
        /* 与 CYD 保持同一约定：交换后 *x 恒为屏幕水平（长）轴原始值、
           *y 为垂直（短）轴原始值。轴向/镜像最终由两点校准吸收 */
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
        ESP_LOGW(TAG, "touch cal file invalid, rewriting factory default");
    } else {
        /* 首次启动：写入出厂默认值（真机提取），不进校准流程；
           个体偏差大时可串口 CLI caltouch 重新校准 */
        ESP_LOGW(TAG, "touch cal not found, writing factory default");
    }
    /* 缺失或损坏：tcal 已是出厂默认值，落盘即可 */
    touch_cal_save();
    return true;
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
/* esp_lcd_panel_draw_bitmap 是 DMA 异步传输：必须等 on_color_trans_done 再释放/复用
   像素缓冲，否则 DMA 读到被覆写的内存，画面出现 Y 向条状撕裂 */
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
    /* ST7796 走 SPI 要求先发像素高字节。就地交换→推→等 DMA 完成→交换还原，
       不拷临时副本：经典核无 PSRAM，LVGL 双缓冲后堆里没有 38KB 连续块，
       此前每次 malloc 失败导致开机动画整段静默丢弃（白屏+残带） */
    uint16_t *p = (uint16_t *)px;   /* 还原后内容不变，语义上仍是 const */
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    xSemaphoreTake(lcd_trans_done, 0);   /* 排掉 LVGL flush 可能留下的存量信号 */
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, p);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lcd_push draw err %s: %d,%d %dx%d", esp_err_to_name(err), x, y, w, h);
    } else if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "lcd_push wait timeout: %d,%d %dx%d", x, y, w, h);
    }
    for (size_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));   /* 还原 */
}

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------- 反色 / 180° 旋转（运行时生效，设置项由 app 层落盘/回读） ---------- */
static bool disp_rot180;

bool bsp_disp_can_invert(void)   { return true; }
bool bsp_disp_can_rotate180(void) { return true; }

void bsp_disp_set_invert(bool en)
{
    if (panel_handle) esp_lcd_panel_invert_color(panel_handle, en);
}

void bsp_disp_set_rotate180(bool en)
{
    disp_rot180 = en;
    /* 默认 mirror(true,true)；180° = 两轴都翻 → mirror(false,false)。
       swap_xy 下 mirror 参数仍指面板轴，两轴同翻与 swap 无关 */
    if (panel_handle) esp_lcd_panel_mirror(panel_handle, !en, !en);
}

/* 背光亮度 0-100（0 也会留 5% 兜底，避免黑屏后摸不到设置） */
static uint8_t bl_duty = 255;
static int     bl_pct = 100;

void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0 && pct < 5) pct = 5;
    bl_pct = pct;
    bl_duty = (uint8_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------- 自动息屏：超时灭背光，触摸唤醒 ---------- */
static uint32_t so_after_s;                     /* 0 = 永不 */
static bool     screen_off;
static int64_t  last_act_us;

void bsp_set_screen_timeout(uint32_t sec)
{
    so_after_s = sec;
    last_act_us = esp_timer_get_time();
    if (screen_off) {                           /* 改设置时若正息屏，先唤醒 */
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_activity(void)               /* 触摸回调里打点 + 唤醒 */
{
    last_act_us = esp_timer_get_time();
    if (screen_off) {
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_off_check(void)              /* lvgl 任务里周期检查 */
{
    if (screen_off || !so_after_s) return;
    if (esp_timer_get_time() - last_act_us > (int64_t)so_after_s * 1000000) {
        screen_off = true;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
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
    static uint16_t black[LCD_H_RES * 40];   /* 静态零初始化即全黑（RGB565 0x0000） */
    for (int y = 0; y < LCD_V_RES; y += 40) {
        xSemaphoreTake(lcd_trans_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 40, black);
        xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    }
}

/* ---------- LVGL 对接 ---------- */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ST7796 走 SPI 要求先发像素高字节，LVGL 内存是小端 RGB565 → 就地交换字节。
       （不用 LV_COLOR_FORMAT_RGB565_SWAPPED：该格式在本版 LVGL 渲染路径上有问题，实测雪花屏） */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
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
        if (screen_off) wake_swallow = true;    /* 息屏时的按下只为唤醒 */
        screen_activity();          /* 息屏唤醒 + 重置超时计时 */
        last_rx = rx;
        last_ry = ry;
        last_valid_us = esp_timer_get_time();
        pressing = true;
    } else if (pressing && esp_timer_get_time() - last_valid_us < 50 * 1000) {
        /* 滑动中压力/采样瞬时丢失（Z 阈值或有效采样数不足）时，桥接为仍按住。
           否则 LVGL 看到 PRESSED→RELEASED 跳变，会把滑动手势拆成一串点按 */
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
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = LV_CLAMP(0, sx, LCD_H_RES - 1);
    data->point.y = LV_CLAMP(0, sy, LCD_V_RES - 1);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        bsp_lvgl_lock();
        lv_timer_handler();
        bsp_lvgl_unlock();
        screen_off_check();
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

    /* LittleFS：挂载 storage 分区到 /littlefs，存放 touch.json 触摸校准参数
       （storage 分区出厂为空，首次启动自动格式化） */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));

    /* 背光：LEDC PWM（GPIO27，高电平点亮），亮度由 bsp_set_brightness 调节 */
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

    /* SPI2 总线（LCD + 触摸共用，厂商引脚分配如此） */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO + ST7796 */
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

    /* 传输完成信号量：bsp_lcd_push 等待 DMA 完成用 */
    lcd_trans_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_color_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,   /* -1：与 EN 共用，reset() 走软件复位 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   /* 面板 BGR 原生（对照 TFT_eSPI ST7796 驱动） */
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io_handle, &panel_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    /* 真机实测：本板 ST7796 不需要开反色，开了颜色呈底片效果；
       反色做成运行时开关（bsp_disp_set_invert），这里保持默认关闭 */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    /* 横屏 480x320；mirror 组合未经真机验证，画面若颠倒改这里 */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
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

    /* 校准文件缺失/损坏时 touch_cal_load 自动落盘出厂默认值（真机提取）；
       仅当默认值也不合用时，串口 CLI caltouch 进两点校准（LVGL 任务启动前手动泵帧） */
    if (!touch_cal_load()) {
        touch_cal_run();
    }

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "BSP ready (E32R35T, %dx%d)", LCD_H_RES, LCD_V_RES);
}

void bsp_restart(void)
{
    /* 先把「重启中」toast 画出来再重启 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

#endif /* CONFIG_BOARD_E32R35T */
