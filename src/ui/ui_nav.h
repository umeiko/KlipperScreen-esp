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

/* Register a semantic control with the nearest panel/modal scope. */
void ui_nav_register_obj(lv_obj_t *obj);

/* One persistent control (currently the title-bar Back button). */
void ui_nav_set_global_obj(lv_obj_t *obj, bool enabled);

/* Modal scopes temporarily own encoder/keypad input, then restore the page. */
lv_group_t *ui_nav_modal_begin(void);
void ui_nav_modal_end(lv_group_t *group);

#ifdef __cplusplus
}
#endif
