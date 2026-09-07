#include "ui_app.h"
#include "panel_mgr.h"
#include "titlebar.h"
#include "printer.h"
#include "ui_anim.h"
#include "theme.h"
#include "lang.h"

void ui_app_create(void)
{
    ui_layout_init();      /* 分辨率/缩放档位，须先于一切 UI 构建 */
    ui_lang_load();        /* klipperscreen.conf 的语言偏好，须先于任何 UI 构建 */
    theme_load();          /* klipperscreen.conf 的主题偏好，须先于任何 UI 构建 */
    titlebar_init();       /* 常驻标题栏（layer_top），必须先于 panel_mgr_init */
    panel_mgr_init();      /* 加载主面板 */
    printer_set_refresh_hook(panel_mgr_tick);   /* 数据层 → UI 刷新回调 */
    printer_init();        /* 数据节拍：esp32=真实 Moonraker 模型，desktop=mock */
}

void ui_app_open(const char *panel)
{
    panel_mgr_open(panel);
}
