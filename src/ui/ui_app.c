#include "ui_app.h"
#include "panel_mgr.h"
#include "titlebar.h"
#include "printer.h"
#include "ui_anim.h"
#include "theme.h"
#include "lang.h"
#include "ui_nav.h"
#include "ui_buttons.h"
#include "bsp_caps.h"
#if BSP_HAS_TIMEZONE_SETTINGS
#include "app_timezone.h"
#endif


void ui_app_create(void)
{
#if BSP_HAS_TIMEZONE_SETTINGS
    app_timezone_apply(app_timezone_current()); /* before clock/network workers start */
#endif
    ui_layout_init();      /* 分辨率/缩放档位，须先于一切 UI 构建 */
    ui_lang_load();        /* klipperscreen.conf 的语言偏好，须先于任何 UI 构建 */
    theme_load();          /* klipperscreen.conf 的主题偏好，须先于任何 UI 构建 */
    ui_nav_init();         /* 编码器/键盘焦点域；触摸输入不受影响 */
    ui_buttons_init();     /* 语义实体按钮层的共享 keypad indev（物理后端在 BSP 侧接入） */
    titlebar_init();       /* 常驻标题栏（layer_top），必须先于 panel_mgr_init */
    panel_mgr_init();      /* 加载主面板 */
    printer_set_refresh_hook(panel_mgr_tick);   /* 数据层 → UI 刷新回调 */
    printer_init();        /* ESP32/Windows=真实 Moonraker；simulator=mock */
}

void ui_app_open(const char *panel)
{
    panel_mgr_open(panel);
}
