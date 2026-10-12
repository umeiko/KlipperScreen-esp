#pragma once

/*
 * Focus navigation for non-pointer inputs (rotary encoders and keypads).
 * Pointer/touch inputs are deliberately left ungrouped, so both interaction
 * styles can be used at the same time.
 */
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_nav_init(void);

lv_group_t *ui_nav_group_create(void);
void ui_nav_prepare_group(lv_group_t *group);
void ui_nav_attach_scope(lv_obj_t *root, lv_group_t *group);
void ui_nav_detach_scope(lv_obj_t *root);
void ui_nav_activate(lv_group_t *group);
/* 销毁一个面板的导航组（panel_mgr 销毁子面板时用）：
   摘除 scope 注册；若该组仍是活动组则先解绑 indev。
   LVGL 的 lv_group_delete 会把组内对象的回指清空，随后删屏幕对象树是安全的。 */
void ui_nav_group_destroy(lv_obj_t *root, lv_group_t *group);
/* Move focus away from a control that the page hid while refreshing itself. */
void ui_nav_refocus_visible(lv_group_t *group);

/* Register a semantic control with the nearest panel/modal scope. */
void ui_nav_register_obj(lv_obj_t *obj);

/* 方向键策略白名单（面板在 create() 里对默认组调用；弹层和未标记组保持原生：
   上下线性移动焦点、左右原样送达控件、回车确认、ESC 返回）：
   - spatial：四方向键按屏幕坐标几何就近聚焦（主界面/温度/数字键盘等网格布局），
     该方向无候选时焦点不动；
   - list：纯列表页，左 = 返回、右 = 进入/确定。 */
void ui_nav_group_set_spatial(lv_group_t *group, bool en);
bool ui_nav_group_is_spatial(lv_group_t *group);
void ui_nav_group_set_list(lv_group_t *group, bool en);
bool ui_nav_group_is_list(lv_group_t *group);
void ui_nav_spatial_move(lv_group_t *group, uint32_t lv_key_dir);

/* One persistent control (currently the title-bar Back button). */
void ui_nav_set_global_obj(lv_obj_t *obj, bool enabled);

/* Modal scopes temporarily own encoder/keypad input, then restore the page. */
lv_group_t *ui_nav_modal_begin(void);
void ui_nav_modal_end(lv_group_t *group);

/* 语义「返回」键的支撑接口（ui_buttons 层用）：
   弹层属主在 modal_begin 后注册自己的「取消/关闭」路径；未注册的弹层收到
   LV_EVENT_CANCEL 兜底。cancel_top 返回是否有弹层消费了本次返回。 */
typedef void (*ui_nav_cancel_cb_t)(void);
void ui_nav_modal_set_cancel(lv_group_t *group, ui_nav_cancel_cb_t cb);
bool ui_nav_modal_cancel_top(void);
lv_group_t *ui_nav_active_group(void);

/* 桌面物理键盘文本输入会话进行中（textarea/回调任一）：方向/确定/返回键
   归会话所有，导航后端应忽略键盘。ESP32 恒 false。 */
bool ui_desktop_input_active(void);

/* Desktop physical-keyboard bridge. ESP32 builds keep these as no-ops, so the
 * touchscreen/encoder virtual keyboards retain their existing behavior. */
typedef enum {
    UI_DESKTOP_INPUT_TEXT = 0,
    UI_DESKTOP_INPUT_BACKSPACE,
    UI_DESKTOP_INPUT_DELETE,
    UI_DESKTOP_INPUT_READY,
    UI_DESKTOP_INPUT_CANCEL,
} ui_desktop_input_event_t;
typedef void (*ui_desktop_input_cb_t)(ui_desktop_input_event_t event,
                                      const char *text, void *user_data);

void ui_desktop_textarea_begin(lv_obj_t *textarea);
void ui_desktop_input_begin(ui_desktop_input_cb_t callback, void *user_data);
void ui_desktop_input_end(void);

#ifdef __cplusplus
}
#endif
