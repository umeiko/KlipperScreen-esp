#include "screensaver.h"
#include "lvgl.h"
#include "theme.h"
#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_wifi.h"
#include "app_settings.h"
#include "weather.h"
#include "lunar.h"
#include "assets/icons.h"
#include "assets/weather_icons.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* 充电触发 / 退出电压（与旧版一致） */
#define SAVER_CHG_MV    4150
#define SAVER_EXIT_MV   4100
#define SAVER_BG_SOLID  0x5BA9E6   /* 纯色背景（天蓝） */
#define SAVER_TXT_TIME  0xFFD500   /* 时间：明黄 */
#define SAVER_TXT_MAIN  0x14305C   /* 深蓝黑（备用） */
#define SAVER_TXT_DATE  0x1F6FB4   /* 日期/预报标签：亮蓝 */
#define SAVER_TXT_LUNAR 0x9A6B2F   /* 农历：赭金 */
#define SAVER_TXT_CITY  0x2F7D4F   /* 城市：墨绿 */
#define SAVER_TXT_DESC  0x4A5A72   /* 天气描述：灰蓝 */
#define SAVER_TXT_TEMP  0xE05A1F   /* 温度：橙红（醒目） */
#define SAVER_TXT_DIM   0x44505C   /* 次要文字：深灰蓝 */

/* 整屏背景随当前天气变化（手机天气 App 风格），索引见 weather_icon_t */
static const uint32_t s_bg_by_wx[7] = {
    0x66B5F0,  /* 0 晴  晴空蓝 */
    0x8EC8F2,  /* 1 多云 浅蓝 */
    0xA2B4C8,  /* 2 阴  灰蓝 */
    0x7E93AC,  /* 3 雨  深灰蓝 */
    0xC9DAEA,  /* 4 雪  青灰白 */
    0xB9C5D2,  /* 5 雾  浅灰 */
    0x5BA9E6,  /* 6 未知 默认天蓝 */
};
#define SAVER_TIME_COL_2 0xFFC531  /* 分针金黄 */

/* 模拟表盘参数（右侧，顶部与首排文字 y12 齐平） */
#define CLK_CX   248
#define CLK_CY   68
#define CLK_R     56

LV_FONT_DECLARE(font_cjk_14);
LV_FONT_DECLARE(font_cjk_16);
LV_FONT_DECLARE(font_cjk_28);
LV_FONT_DECLARE(font_cjk_32);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_48);

static lv_obj_t *scr_layer;
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_lunar;
static lv_obj_t *lbl_city;
static lv_obj_t *lbl_desc;
static lv_obj_t *lbl_temp;
static lv_obj_t *img_wx;
static lv_obj_t *img_wifi;
static lv_obj_t *fx_img[3];
static lv_obj_t *fx_text[3];
static lv_obj_t *line_hour;
static lv_obj_t *line_min;
static lv_obj_t *line_sec;
static lv_point_precise_t pt_hour[2], pt_min[2], pt_sec[2];
static lv_timer_t *clock_timer;
static int active;
static uint64_t last_wx_upd;

static const lv_image_dsc_t *wx_icons[] = {
    &img_weather_sun, &img_weather_partly, &img_weather_overcast,
    &img_weather_rain, &img_weather_snow, &img_weather_fog,
    &img_weather_overcast,   /* UNKNOWN 兜底 */
};

static const char *wx_desc[] = {
    "晴", "多云", "阴", "雨", "雪", "雾", "--",
};

static const char *wday_cn[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

/* 农历月/日转中文（九月初十 / 腊月初二 / 闰二月十五） */
static void fmt_lunar(const lunar_t *l, char *buf, size_t n)
{
    static const char *mon_num[11] = { "一", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬" };
    const char *m;
    if (l->month == 1)       m = "正";
    else if (l->month == 12) m = "腊";
    else if (l->month <= 10) m = mon_num[l->month - 1];
    else                     m = "十一";
    char dbuf[8];
    if (l->day == 10)            snprintf(dbuf, sizeof(dbuf), "初十");
    else if (l->day == 20)       snprintf(dbuf, sizeof(dbuf), "二十");
    else if (l->day == 30)       snprintf(dbuf, sizeof(dbuf), "三十");
    else if (l->day < 10)        snprintf(dbuf, sizeof(dbuf), "初%s", mon_num[l->day - 1]);
    else if (l->day < 20)        snprintf(dbuf, sizeof(dbuf), "十%s", mon_num[l->day - 10 - 1]);
    else                         snprintf(dbuf, sizeof(dbuf), "二十%s", mon_num[l->day - 20 - 1]);
    snprintf(buf, n, "%s%s月%s", l->leap ? "闰" : "", m, dbuf);
}

/* 更新模拟时钟指针：时针 / 分针 / 秒针 */
static void update_hands(int hour, int min, int sec)
{
    double ha = (hour % 12 + min / 60.0) * M_PI / 6.0;
    double ma = (min + sec / 60.0) * M_PI / 30.0;
    double sa = sec * M_PI / 30.0;

    pt_hour[0].x = CLK_CX;  pt_hour[0].y = CLK_CY;
    pt_hour[1].x = CLK_CX + (lv_coord_t)(CLK_R * 0.48 * sin(ha));
    pt_hour[1].y = CLK_CY - (lv_coord_t)(CLK_R * 0.48 * cos(ha));

    pt_min[0].x = CLK_CX;   pt_min[0].y = CLK_CY;
    pt_min[1].x = CLK_CX + (lv_coord_t)(CLK_R * 0.76 * sin(ma));
    pt_min[1].y = CLK_CY - (lv_coord_t)(CLK_R * 0.76 * cos(ma));

    pt_sec[0].x = CLK_CX;   pt_sec[0].y = CLK_CY;
    pt_sec[1].x = CLK_CX + (lv_coord_t)(CLK_R * 0.82 * sin(sa));
    pt_sec[1].y = CLK_CY - (lv_coord_t)(CLK_R * 0.82 * cos(sa));

    lv_line_set_points(line_hour, pt_hour, 2);
    lv_line_set_points(line_min,  pt_min,  2);
    lv_line_set_points(line_sec,  pt_sec,  2);
}

static void update_clock(void)
{
    time_t t = time(NULL);
    if (t < 1767225600) {   /* 2026-01-01 之前视为未同步 */
        lv_label_set_text(lbl_time, "--:--");
        lv_label_set_text(lbl_date, "--年--月--日 --");
        lv_label_set_text(lbl_lunar, "--");
        update_hands(0, 0, 0);
        return;
    }
    struct tm *tmv = localtime(&t);
    if (!tmv) return;

    /* 样板样式：24h 大数字，无 AM/PM */
    lv_label_set_text_fmt(lbl_time, "%02d:%02d", tmv->tm_hour, tmv->tm_min);
    /* 第二行：年月日 + 星期 */
    lv_label_set_text_fmt(lbl_date, "%d年%d月%d日 %s",
                          tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                          wday_cn[tmv->tm_wday]);

    lunar_t lu;
    solar_to_lunar(tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday, &lu);
    char lstr[16];
    fmt_lunar(&lu, lstr, sizeof(lstr));
    lv_label_set_text_fmt(lbl_lunar, "农历 %s", lstr);

    update_hands(tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
}

static void update_weather(void)
{
    weather_data_t wd;
    bool ok = weather_get(&wd);
    if (!ok) {
        char cname[16];
        settings_load_weather_city_name(cname, sizeof(cname));
        lv_label_set_text(lbl_city, cname[0] ? cname : "--");
        lv_label_set_text(lbl_desc, "天气未配置");
        lv_obj_set_style_text_color(lbl_desc, lv_color_hex(SAVER_TXT_DESC), 0);
        lv_label_set_text(lbl_temp, "--°");
        lv_obj_set_style_bg_color(scr_layer, lv_color_hex(SAVER_BG_SOLID), 0);
        lv_obj_add_flag(img_wx, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            lv_obj_add_flag(fx_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_text[i], "--");
        }
        return;
    }
    if (wd.updated_ms == last_wx_upd) return;
    last_wx_upd = wd.updated_ms;

    char cname[16];
    settings_load_weather_city_name(cname, sizeof(cname));
    lv_label_set_text(lbl_city, (cname[0] || wd.city[0]) ? (cname[0] ? cname : wd.city) : "--");

    weather_icon_t ic = weather_icon_from_code(wd.icon);
    lv_img_set_src(img_wx, wx_icons[ic]);
    lv_obj_remove_flag(img_wx, LV_OBJ_FLAG_HIDDEN);
    /* 整屏背景随当前天气变化 */
    lv_obj_set_style_bg_color(scr_layer, lv_color_hex(s_bg_by_wx[ic]), 0);
    /* 天气描述用高德原文（更精确），否则用归并名 */
    lv_label_set_text(lbl_desc, wd.category[0] ? wd.category : wx_desc[ic]);
    lv_obj_set_style_text_color(lbl_desc, lv_color_hex(SAVER_TXT_DESC), 0);

    lv_label_set_text_fmt(lbl_temp, "%d°", wd.temp_c);

    /* 三天预报：今天/明天/后天，大图标 + 底部一行"标签 高/低温" */
    static const char *fx_tag_txt[3] = { "今天", "明天", "后天" };
    for (int i = 0; i < 3; i++) {
        if (i < wd.forecast_count && wd.forecast[i].text[0]) {
            lv_img_set_src(fx_img[i], wx_icons[wd.forecast[i].icon < WEATHER_UNKNOWN ? wd.forecast[i].icon : WEATHER_UNKNOWN]);
            lv_obj_remove_flag(fx_img[i], LV_OBJ_FLAG_HIDDEN);
            if (wd.forecast[i].tmax > -90 && wd.forecast[i].tmin > -90)
                lv_label_set_text_fmt(fx_text[i], "%s %d°/%d°", fx_tag_txt[i], wd.forecast[i].tmax, wd.forecast[i].tmin);
            else
                lv_label_set_text(fx_text[i], fx_tag_txt[i]);
        } else {
            lv_obj_add_flag(fx_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(fx_text[i], "--");
        }
    }
}

static void on_clock(lv_timer_t *t)
{
    (void)t;
    update_clock();
    update_weather();
    /* WiFi 图标颜色：绿=已连，橙=连接中，白=未连 */
    uint32_t col = THEME_COL_TEXT;
    if (bsp_wifi_status() == BSP_WIFI_CONNECTING) col = THEME_COL_WARN;
    else if (bsp_wifi_connected())                col = THEME_COL_OK;
    lv_obj_set_style_image_recolor(img_wifi, theme_col(col), 0);
}

static void show(void)
{
    if (active) return;
    active = 1;
    last_wx_upd = 0;
    lv_obj_remove_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(clock_timer);
    on_clock(clock_timer);   /* 立即刷新一次 */
}

static void hide(void)
{
    if (!active) return;
    active = 0;
    lv_obj_add_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(clock_timer);
}

/* 表盘 12 个数字刻度（固定位置） */
static void create_dial_ticks(lv_obj_t *parent)
{
    static const char *num[12] = { "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11" };
    for (int k = 0; k < 12; k++) {
        double a = k * M_PI / 6.0;
        lv_coord_t x = CLK_CX + (lv_coord_t)((CLK_R - 15) * sin(a));
        lv_coord_t y = CLK_CY - (lv_coord_t)((CLK_R - 15) * cos(a));
        lv_obj_t *lb = lv_label_create(parent);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lb, lv_color_white(), 0);
        lv_label_set_text(lb, num[k]);
        /* 两位数字（10/11/12）稍宽，微调居中 */
        lv_coord_t off = (k == 0 || k >= 10) ? 18 : 9;
        lv_obj_align(lb, LV_ALIGN_TOP_LEFT, x - off, y - 9);
    }
}

void screensaver_init(void)
{
    weather_start();   /* 后台天气任务（幂等） */

    scr_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr_layer, 320, 240);
    lv_obj_align(scr_layer, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(scr_layer, lv_color_hex(SAVER_BG_SOLID), 0);
    lv_obj_set_style_border_width(scr_layer, 0, 0);
    lv_obj_set_style_pad_all(scr_layer, 0, 0);
    lv_obj_remove_flag(scr_layer, LV_OBJ_FLAG_SCROLLABLE);

    /* 背景：纯色（SAVER_BG_SOLID，见上方 scr_layer 底色） */

    /* ---- 左侧：大号时间（24h）---- */
    lbl_time = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(SAVER_TXT_TIME), 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_LEFT, 16, 12);

    /* 日期（中文样式）：周六 10月3日 */
    lbl_date = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_date, &font_cjk_16, 0);
    lv_obj_set_style_text_color(lbl_date, lv_color_hex(SAVER_TXT_DATE), 0);
    lv_label_set_text(lbl_date, "--.--.--");
    lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, 18, 66);

    /* 农历行 */
    lbl_lunar = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_lunar, &font_cjk_14, 0);
    lv_obj_set_style_text_color(lbl_lunar, lv_color_hex(SAVER_TXT_LUNAR), 0);
    lv_label_set_text(lbl_lunar, "--");
    lv_obj_align(lbl_lunar, LV_ALIGN_TOP_LEFT, 18, 88);

    /* 城市 */
    lbl_city = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_city, &font_cjk_16, 0);
    lv_obj_set_style_text_color(lbl_city, lv_color_hex(SAVER_TXT_CITY), 0);
    lv_label_set_text(lbl_city, "--");
    lv_obj_align(lbl_city, LV_ALIGN_TOP_LEFT, 18, 108);

    /* 天气描述（实时，放图标下方） */
    lbl_desc = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_desc, &font_cjk_14, 0);
    lv_obj_set_style_text_color(lbl_desc, lv_color_hex(SAVER_TXT_DESC), 0);
    lv_label_set_text(lbl_desc, "");
    lv_obj_align(lbl_desc, LV_ALIGN_TOP_LEFT, 14, 168);

    /* 天气图标 + 大温度（高低温见三天预报，不再单独显示） */
    img_wx = lv_img_create(scr_layer);
    lv_img_set_src(img_wx, &img_weather_overcast);
    lv_obj_align(img_wx, LV_ALIGN_TOP_LEFT, 14, 126);

    lbl_temp = lv_label_create(scr_layer);
    lv_obj_set_style_text_font(lbl_temp, &font_cjk_32, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(SAVER_TXT_TEMP), 0);
    lv_label_set_text(lbl_temp, "--°");
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_LEFT, 68, 130);

    /* ---- 右上 WiFi 图标 ---- */
    img_wifi = lv_img_create(scr_layer);
    lv_img_set_src(img_wifi, &img_wifi_4);
    lv_obj_set_style_image_recolor(img_wifi, lv_color_hex(SAVER_TXT_DIM), 0);
    lv_obj_set_style_image_recolor_opa(img_wifi, LV_OPA_COVER, 0);
    lv_obj_align(img_wifi, LV_ALIGN_TOP_RIGHT, -14, 8);

    /* ---- 右侧模拟时钟 ---- */
    lv_obj_t *face = lv_obj_create(scr_layer);
    lv_obj_set_size(face, CLK_R * 2, CLK_R * 2);
    lv_obj_align(face, LV_ALIGN_TOP_LEFT, CLK_CX - CLK_R, CLK_CY - CLK_R);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x0A2244), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_50, 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_border_color(face, lv_color_white(), 0);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    create_dial_ticks(face);

    line_hour = lv_line_create(scr_layer);
    lv_obj_set_style_line_width(line_hour, 3, 0);
    lv_obj_set_style_line_color(line_hour, lv_color_white(), 0);
    lv_line_set_points(line_hour, pt_hour, 2);

    line_min = lv_line_create(scr_layer);
    lv_obj_set_style_line_width(line_min, 2, 0);
    lv_obj_set_style_line_color(line_min, lv_color_hex(SAVER_TIME_COL_2), 0);
    lv_line_set_points(line_min, pt_min, 2);

    line_sec = lv_line_create(scr_layer);
    lv_obj_set_style_line_width(line_sec, 1, 0);
    lv_obj_set_style_line_color(line_sec, lv_color_hex(0xFF4444), 0);
    lv_line_set_points(line_sec, pt_sec, 2);

    lv_obj_t *hub = lv_obj_create(scr_layer);
    lv_obj_set_size(hub, 8, 8);
    lv_obj_align(hub, LV_ALIGN_TOP_LEFT, CLK_CX - 4, CLK_CY - 4);
    lv_obj_set_style_bg_color(hub, lv_color_white(), 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 三天预报：底部三卡（大图标 + 标签/温度行）---- */
    for (int i = 0; i < 3; i++) {
        lv_coord_t cx = 53 + i * 107;

        fx_img[i] = lv_img_create(scr_layer);
        lv_img_set_src(fx_img[i], &img_weather_overcast);
        lv_image_set_scale(fx_img[i], 256);   /* 与实时天气图标同尺寸（48px） */
        lv_obj_align(fx_img[i], LV_ALIGN_TOP_LEFT, cx - 24, 176);

        fx_text[i] = lv_label_create(scr_layer);
        lv_obj_set_style_text_font(fx_text[i], &font_cjk_14, 0);
        lv_obj_set_style_text_color(fx_text[i], lv_color_hex(SAVER_TXT_DATE), 0);
        lv_label_set_text(fx_text[i], "--");
        lv_obj_align(fx_text[i], LV_ALIGN_TOP_LEFT, cx - 52, 226);
    }

    clock_timer = lv_timer_create(on_clock, 1000, NULL);
    lv_timer_pause(clock_timer);

    lv_obj_add_flag(scr_layer, LV_OBJ_FLAG_HIDDEN);
}

int screensaver_is_active(void) { return active; }

void screensaver_poll(void)
{
    uint64_t idle = bsp_screen_idle_ms();
    int mv = bsp_battery_mv();

    if (active) {
        /* 任意输入唤醒即退出屏保 */
        if (idle < 200) { hide(); return; }
        /* 电压回落到阈值以下也退出 */
        if (mv <= SAVER_EXIT_MV) hide();
        return;
    }
    /* 屏幕已熄则不播（息屏优先级更高） */
    if (bsp_screen_is_off()) return;
    if (mv > SAVER_CHG_MV && idle >= (uint64_t)settings_load_saver_delay() * 1000U)
        show();
}
