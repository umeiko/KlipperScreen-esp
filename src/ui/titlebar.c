#include "titlebar.h"
#include "theme.h"
#include "lang.h"
#include "assets/icons.h"
#include "panel_mgr.h"
#include "printer.h"
#include "bsp_wifi.h"
#include "widgets/confirm.h"
#include "ui_anim.h"
#include <time.h>

static lv_obj_t *bar;
static lv_obj_t *btn_back;
static lv_obj_t *btn_motoroff;
static lv_obj_t *lbl_title;
static lv_obj_t *lbl_wifi;
static lv_obj_t *lbl_ext;
static lv_obj_t *lbl_bed;
static lv_obj_t *ic_ext;
static lv_obj_t *ic_bed;
static lv_obj_t *lbl_tool;   /* 多工具时活动工具指示 T{n} */
static int show_clock;   /* 主面板（无返回键且无标题）→ 标题位显示时钟 */
static int show_temps = 1;   /* 标题长的面板（Moonraker 设置等）可关掉温度显示 */
static int title_x;      /* 标题左缘 x（有无返回键两种布局） */

/* 标题可用宽度 = 屏幕宽 - 左缘 - 右侧预留（温度区约 124px / 电机键 56px） */
static void title_update_width(void)
{
    int reserve = ui_px(8);
    if (show_temps) reserve = ui_px(140);   /* 124 + 活动工具 T{n} 指示 */
    else if (!(lv_obj_has_flag(btn_motoroff, LV_OBJ_FLAG_HIDDEN))) reserve = ui_px(56);
    lv_obj_set_width(lbl_title, ui_scr_w() - title_x - reserve);
}

static void back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    panel_mgr_back();
}

static void do_motoroff(void *ud)
{
    LV_UNUSED(ud);
    printer_motors_off();   /* M84 */
    ui_toast("已关闭电机（M84）", THEME_COL_WARN);
}

static void motoroff_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    confirm_open("确认关闭电机？\n所有步进电机将失去保持力矩（M84）", "关闭电机", do_motoroff, NULL);
}

void titlebar_init(void)
{
    lv_obj_t *top = lv_layer_top();

    bar = lv_obj_create(top);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, ui_scr_w(), THEME_TITLEBAR_H);
    lv_obj_set_style_bg_color(bar, theme_col(THEME_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, ui_px(6), 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    btn_back = theme_button(bar, LV_SYMBOL_LEFT, NULL, 0);
    lv_obj_set_size(btn_back, ui_px(64), THEME_TITLEBAR_H - ui_px(4));   /* 宽一点好点（电阻屏精度差） */
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    /* 右上角关闭电机按钮（M84）：默认隐藏，仅移动面板经 titlebar_show_motoroff 打开 */
    btn_motoroff = theme_button(bar, NULL, NULL, 0);
    lv_obj_set_size(btn_motoroff, ui_px(44), THEME_TITLEBAR_H - ui_px(4));
    lv_obj_align(btn_motoroff, LV_ALIGN_RIGHT_MID, -ui_px(2), 0);
    lv_obj_add_event_cb(btn_motoroff, motoroff_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic_motor = theme_img(btn_motoroff, ui_icon(&img_motor_off, &img_motor_off_36), THEME_COL_TEXT);
    /* 真彩色图标（浅灰电机+红叉）：取消 theme_img 的 recolor，保留本色 */
    lv_obj_set_style_image_recolor_opa(ic_motor, LV_OPA_TRANSP, 0);
    lv_obj_center(ic_motor);
    lv_obj_add_flag(btn_motoroff, LV_OBJ_FLAG_HIDDEN);

    /* WiFi 连接状态小图标（左侧，返回键之后）。
       ESP32 的 WiFi/PHY 启动可能阻塞或短暂打断 C3 原生 USB，不能在持有
       LVGL 锁的 UI 构造路径中执行；ESP32 entry 会在 UI 就绪并解锁后初始化。 */
#ifndef ESP_PLATFORM
    bsp_wifi_init();
#endif
    lbl_wifi = theme_label(bar, LV_SYMBOL_WIFI, THEME_FONT_ICON, THEME_COL_TEXT_DIM);
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, ui_px(46), 0);

    lbl_title = theme_label(bar, "", THEME_FONT_M, THEME_COL_TEXT);
    /* 标题限宽 + 超长滚动：法/意等长标题不能盖住右侧温度区 */
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, ui_px(72), 0);

    /* 右侧：喷嘴/热床实时温度（小图标 + 数值）。
       温度标签定宽 + 文本右对齐，图标/标签用 OUT_LEFT_MID 从右往左链式排布：
       百位温度（"119°"）变宽时向左占定宽内的空间，不会再压住左侧图标。 */
    lbl_bed = theme_label(bar, "", THEME_FONT_S, THEME_COL_BED);
    lv_obj_set_width(lbl_bed, ui_px(36));
    lv_obj_set_style_text_align(lbl_bed, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_bed, LV_ALIGN_RIGHT_MID, -ui_px(2), 0);
    ic_bed = theme_img(bar, ui_icon(&img_bed_16, &img_bed_32), THEME_COL_BED);
    lv_obj_align_to(ic_bed, lbl_bed, LV_ALIGN_OUT_LEFT_MID, -ui_px(2), 0);
    lbl_ext = theme_label(bar, "", THEME_FONT_S, THEME_COL_EXTRUDER);
    lv_obj_set_width(lbl_ext, ui_px(36));
    lv_obj_set_style_text_align(lbl_ext, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(lbl_ext, ic_bed, LV_ALIGN_OUT_LEFT_MID, -ui_px(8), 0);
    ic_ext = theme_img(bar, ui_icon(&img_nozzle_16, &img_nozzle_32), THEME_COL_EXTRUDER);
    lv_obj_align_to(ic_ext, lbl_ext, LV_ALIGN_OUT_LEFT_MID, -ui_px(2), 0);
    /* 多工具：喷嘴图标左侧的小号 T{n}，单挤出机时隐藏 */
    lbl_tool = theme_label(bar, "", THEME_FONT_S, THEME_COL_EXTRUDER);
    lv_obj_align_to(lbl_tool, ic_ext, LV_ALIGN_OUT_LEFT_MID, -ui_px(2), 0);
    lv_obj_add_flag(lbl_tool, LV_OBJ_FLAG_HIDDEN);

    titlebar_tick();
}

lv_obj_t *titlebar_back_button(void) { return btn_back; }
lv_obj_t *titlebar_motoroff_button(void) { return btn_motoroff; }

void titlebar_show_motoroff(int show)
{
    if (show) lv_obj_remove_flag(btn_motoroff, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(btn_motoroff, LV_OBJ_FLAG_HIDDEN);
    title_update_width();
}

void titlebar_show_temps(int show)
{
    show_temps = show;
    lv_obj_t *objs[] = { lbl_ext, lbl_bed, ic_ext, ic_bed };
    for (unsigned i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (show) lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!show) lv_obj_add_flag(lbl_tool, LV_OBJ_FLAG_HIDDEN);
    title_update_width();
}

void titlebar_set(const char *title, int show_back)
{
    show_clock = !show_back && (!title || !title[0]);
    if (!show_clock) lv_label_set_text(lbl_title, ui_tr(title));
    if (show_back) lv_obj_remove_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(btn_back, LV_OBJ_FLAG_HIDDEN);
    /* 无返回键时整体左移 */
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, show_back ? ui_px(72) : ui_px(8), 0);
    title_x = show_back ? ui_px(100) : ui_px(32);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, title_x, 0);
    title_update_width();
}

void titlebar_tick(void)
{
    if (show_clock) {
        /* 主面板标题位显示 HH:MM；SNTP 未同步到时先占位 */
        time_t t = time(NULL);
        if (t < 1767225600) {   /* 2026-01-01，小于此值认为未同步 */
            lv_label_set_text(lbl_title, "--:--");
        } else {
            struct tm *tmv = localtime(&t);   /* 可移植性优先（MinGW 无 localtime_r），LVGL 单线程调用 */
            if (tmv)
                lv_label_set_text_fmt(lbl_title, "%02d:%02d", tmv->tm_hour, tmv->tm_min);
        }
    }
    lv_label_set_text_fmt(lbl_ext, "%d" "\xC2\xB0", (int)(printer_temp_ext() + 0.5f));
    lv_label_set_text_fmt(lbl_bed, "%d" "\xC2\xB0", (int)(printer_temp_bed() + 0.5f));

    /* 多工具：显示活动工具 T{n}（printer_temp_ext 已取该工具温度） */
    if (show_temps && printer_tool_count() > 1) {
        lv_obj_remove_flag(lbl_tool, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(lbl_tool, "T%d", printer_current_tool());
    } else {
        lv_obj_add_flag(lbl_tool, LV_OBJ_FLAG_HIDDEN);
    }

    /* 连接中=橙，已连接=绿，未连接=灰 */
    uint32_t col = THEME_COL_TEXT_DIM;
    if (bsp_wifi_status() == BSP_WIFI_CONNECTING) col = THEME_COL_WARN;
    else if (bsp_wifi_connected())                col = THEME_COL_OK;
    lv_obj_set_style_text_color(lbl_wifi, theme_col(col), 0);
}
