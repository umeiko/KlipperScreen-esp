#include "ui_nav.h"
#include "theme.h"

#ifndef ESP_PLATFORM
#include <SDL.h>
#endif

#define UI_NAV_SCOPE_MAX 24
#define UI_NAV_MODAL_MAX 8

typedef struct {
    lv_obj_t *root;
    lv_group_t *group;
} nav_scope_t;

static nav_scope_t scopes[UI_NAV_SCOPE_MAX];
static lv_group_t *active_group;
static lv_group_t *modal_stack[UI_NAV_MODAL_MAX];
static unsigned modal_depth;
static lv_obj_t *global_obj;
static bool global_enabled;

#ifndef ESP_PLATFORM
static lv_obj_t *desktop_textarea;
static ui_desktop_input_cb_t desktop_input_cb;
static void *desktop_input_ud;
static bool desktop_watch_installed;

static int SDLCALL desktop_keyboard_watch(void *userdata, SDL_Event *event)
{
    LV_UNUSED(userdata);
    if (!desktop_textarea && !desktop_input_cb) return 0;

    if (event->type == SDL_TEXTINPUT) {
        if (desktop_textarea)
            lv_textarea_add_text(desktop_textarea, event->text.text);
        else
            desktop_input_cb(UI_DESKTOP_INPUT_TEXT, event->text.text, desktop_input_ud);
        return 0;
    }
    if (event->type != SDL_KEYDOWN) return 0;

    SDL_Keycode key = event->key.keysym.sym;
    SDL_Keymod mod = (SDL_Keymod)event->key.keysym.mod;
    if ((mod & KMOD_CTRL) && key == SDLK_v) {
        char *clipboard = SDL_GetClipboardText();
        if (clipboard) {
            if (desktop_textarea)
                lv_textarea_add_text(desktop_textarea, clipboard);
            else
                desktop_input_cb(UI_DESKTOP_INPUT_TEXT, clipboard, desktop_input_ud);
            SDL_free(clipboard);
        }
        return 0;
    }

    ui_desktop_input_event_t input_event;
    switch (key) {
    case SDLK_BACKSPACE: input_event = UI_DESKTOP_INPUT_BACKSPACE; break;
    case SDLK_DELETE:    input_event = UI_DESKTOP_INPUT_DELETE;    break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  input_event = UI_DESKTOP_INPUT_READY;     break;
    case SDLK_ESCAPE:    input_event = UI_DESKTOP_INPUT_CANCEL;    break;
    case SDLK_LEFT:
        if (desktop_textarea) lv_textarea_cursor_left(desktop_textarea);
        return 0;
    case SDLK_RIGHT:
        if (desktop_textarea) lv_textarea_cursor_right(desktop_textarea);
        return 0;
    case SDLK_HOME:
        if (desktop_textarea) lv_textarea_set_cursor_pos(desktop_textarea, 0);
        return 0;
    case SDLK_END:
        if (desktop_textarea) lv_textarea_set_cursor_pos(desktop_textarea, LV_TEXTAREA_CURSOR_LAST);
        return 0;
    default:
        return 0;   /* printable keys arrive separately as SDL_TEXTINPUT */
    }

    if (desktop_textarea) {
        if (input_event == UI_DESKTOP_INPUT_BACKSPACE)
            lv_textarea_delete_char(desktop_textarea);
        else if (input_event == UI_DESKTOP_INPUT_DELETE)
            lv_textarea_delete_char_forward(desktop_textarea);
        else if (input_event == UI_DESKTOP_INPUT_READY)
            lv_obj_send_event(desktop_textarea, LV_EVENT_READY, NULL);
        else if (input_event == UI_DESKTOP_INPUT_CANCEL)
            lv_obj_send_event(desktop_textarea, LV_EVENT_CANCEL, NULL);
    } else {
        desktop_input_cb(input_event, NULL, desktop_input_ud);
    }
    return 0;
}

static void desktop_input_start(void)
{
    if (!desktop_watch_installed) {
        SDL_AddEventWatch(desktop_keyboard_watch, NULL);
        desktop_watch_installed = true;
    }
    SDL_StartTextInput();
}
#endif

/*
 * LVGL normally scrolls a newly focused child into view with animation.  That
 * leaves a short window where an encoder can wrap to an item at the other end
 * of a list while the old scroll offset is still visible.  Encoder navigation
 * should track the focus immediately, so the group owns that policy.
 */
static void focus_changed(lv_group_t *group)
{
    lv_obj_t *obj = lv_group_get_focused(group);
    if (!obj || obj == global_obj) return;

    /*
     * The title bar lives on layer_top and visually covers the top of every
     * panel.  LVGL's generic scroll-to-view only knows the screen rectangle,
     * so it can legally place a focused row underneath that overlay.  Walk
     * every scrollable ancestor and keep the focused object inside the real
     * content viewport instead.
     */
    lv_obj_update_layout(obj);
    for (lv_obj_t *parent = lv_obj_get_parent(obj); parent;
         parent = lv_obj_get_parent(parent)) {
        if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) continue;

        lv_area_t area, viewport;
        lv_obj_get_coords(obj, &area);
        lv_obj_get_coords(parent, &viewport);

        int32_t top = viewport.y1;
        int32_t bottom = viewport.y2;
        int32_t safe_top = THEME_TITLEBAR_H + ui_px(4);
        int32_t safe_bottom = ui_scr_h() - ui_px(4) - 1;
        if (top < safe_top) top = safe_top;
        if (bottom > safe_bottom) bottom = safe_bottom;

        int32_t dy = 0;
        if (area.y1 < top) dy = top - area.y1;
        else if (area.y2 > bottom) dy = bottom - area.y2;
        if (dy) {
            lv_obj_scroll_by_bounded(parent, 0, dy, LV_ANIM_OFF);
            lv_obj_update_layout(obj);
        }
    }
}

static void bind_navigation_indevs(lv_group_t *group)
{
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_type_t type = lv_indev_get_type(indev);
        if (type == LV_INDEV_TYPE_ENCODER || type == LV_INDEV_TYPE_KEYPAD)
            lv_indev_set_group(indev, group);
    }
}

void ui_nav_init(void)
{
    for (unsigned i = 0; i < UI_NAV_SCOPE_MAX; i++) {
        scopes[i].root = NULL;
        scopes[i].group = NULL;
    }
    active_group = NULL;
    modal_depth = 0;
    global_obj = NULL;
    global_enabled = false;
}

lv_group_t *ui_nav_group_create(void)
{
    lv_group_t *group = lv_group_create();
    if (group) {
        lv_group_set_wrap(group, true);
        lv_group_set_focus_cb(group, focus_changed);
    }
    return group;
}

void ui_nav_prepare_group(lv_group_t *group)
{
    if (group)
        lv_group_set_default(group);
}

void ui_nav_attach_scope(lv_obj_t *root, lv_group_t *group)
{
    if (!root || !group) return;
    for (unsigned i = 0; i < UI_NAV_SCOPE_MAX; i++) {
        if (scopes[i].root == root || scopes[i].root == NULL) {
            scopes[i].root = root;
            scopes[i].group = group;
            return;
        }
    }
}

void ui_nav_detach_scope(lv_obj_t *root)
{
    for (unsigned i = 0; i < UI_NAV_SCOPE_MAX; i++) {
        if (scopes[i].root == root) {
            scopes[i].root = NULL;
            scopes[i].group = NULL;
            return;
        }
    }
}

void ui_nav_register_obj(lv_obj_t *obj)
{
    if (!obj) return;
    /* focus_changed() provides deterministic, non-animated list tracking. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    for (lv_obj_t *node = obj; node; node = lv_obj_get_parent(node)) {
        for (unsigned i = 0; i < UI_NAV_SCOPE_MAX; i++) {
            if (scopes[i].root == node) {
                lv_group_add_obj(scopes[i].group, obj);
                return;
            }
        }
    }

    /* Static page controls are built before the page root can be attached. */
    lv_group_t *group = lv_group_get_default();
    if (group)
        lv_group_add_obj(group, obj);
}

void ui_nav_activate(lv_group_t *group)
{
    if (!group) return;
    if (global_obj)
        lv_group_remove_obj(global_obj);
    active_group = group;
    lv_group_set_default(group);
    bind_navigation_indevs(group);
    if (global_obj && global_enabled && modal_depth == 0)
        lv_group_add_obj(group, global_obj);
}

void ui_nav_refocus_visible(lv_group_t *group)
{
    if (!group) return;
    lv_obj_t *focused = lv_group_get_focused(group);
    if (!focused) {
        lv_group_focus_next(group);
        return;
    }

    bool available = !(lv_obj_get_state(focused) & LV_STATE_DISABLED);
    for (lv_obj_t *node = focused; available && node; node = lv_obj_get_parent(node))
        available = !lv_obj_has_flag(node, LV_OBJ_FLAG_HIDDEN);

    /* Conditional pages often create all possible actions once and hide the
       unavailable ones in on_show().  LVGL leaves focus on such an object;
       advance once and its group walker skips every hidden entry. */
    if (!available)
        lv_group_focus_next(group);
}

void ui_nav_set_global_obj(lv_obj_t *obj, bool enabled)
{
    if (global_obj && global_obj != obj)
        lv_group_remove_obj(global_obj);
    global_obj = obj;
    global_enabled = enabled;
    if (!global_obj) return;
    lv_group_remove_obj(global_obj);
    if (global_enabled && active_group && modal_depth == 0)
        lv_group_add_obj(active_group, global_obj);
}

lv_group_t *ui_nav_modal_begin(void)
{
    if (modal_depth >= UI_NAV_MODAL_MAX) return NULL;
    lv_group_t *group = ui_nav_group_create();
    if (!group) return NULL;
    modal_stack[modal_depth++] = active_group;
    ui_nav_activate(group);
    return group;
}

void ui_nav_modal_end(lv_group_t *group)
{
    if (!group) return;
    lv_group_t *restore = NULL;
    if (active_group == group && modal_depth > 0)
        restore = modal_stack[--modal_depth];

    if (restore)
        ui_nav_activate(restore);
    else if (active_group == group) {
        active_group = NULL;
        lv_group_set_default(NULL);
        bind_navigation_indevs(NULL);
    }
    lv_group_delete(group);
}

void ui_desktop_textarea_begin(lv_obj_t *textarea)
{
#ifndef ESP_PLATFORM
    desktop_textarea = textarea;
    desktop_input_cb = NULL;
    desktop_input_ud = NULL;
    desktop_input_start();
#else
    LV_UNUSED(textarea);
#endif
}

void ui_desktop_input_begin(ui_desktop_input_cb_t callback, void *user_data)
{
#ifndef ESP_PLATFORM
    desktop_textarea = NULL;
    desktop_input_cb = callback;
    desktop_input_ud = user_data;
    desktop_input_start();
#else
    LV_UNUSED(callback);
    LV_UNUSED(user_data);
#endif
}

void ui_desktop_input_end(void)
{
#ifndef ESP_PLATFORM
    desktop_textarea = NULL;
    desktop_input_cb = NULL;
    desktop_input_ud = NULL;
    SDL_StopTextInput();
#endif
}
