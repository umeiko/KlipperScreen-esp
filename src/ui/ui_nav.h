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
/* Move focus away from a control that the page hid while refreshing itself. */
void ui_nav_refocus_visible(lv_group_t *group);

/* Register a semantic control with the nearest panel/modal scope. */
void ui_nav_register_obj(lv_obj_t *obj);

/* One persistent control (currently the title-bar Back button). */
void ui_nav_set_global_obj(lv_obj_t *obj, bool enabled);

/* Modal scopes temporarily own encoder/keypad input, then restore the page. */
lv_group_t *ui_nav_modal_begin(void);
void ui_nav_modal_end(lv_group_t *group);

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
