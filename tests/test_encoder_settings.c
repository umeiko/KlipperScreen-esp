/* Windows preview smoke: real LVGL/UI/settings/SDL backend, no physical encoder.
 * Reuse the simulator entry helpers and display page to exercise its private
 * trial state without adding test commands to the shipped application. */
#include <SDL.h>
#undef main
#define main unused_simulator_main
#include "../src/ports/desktop/main.c"
#undef main
#include "../src/ui/panels/panel_display.c"
#include <assert.h>
#include "bsp_conf.h"

static lv_obj_t *find_encoder_dropdown(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_dropdown_class) &&
        strcmp(lv_dropdown_get_options(root), "1\n2\n4") == 0) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_encoder_dropdown(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

static void choose(int counts)
{
    lv_obj_t *dd = find_encoder_dropdown(lv_screen_active());
    assert(dd);
    assert(encoder_index(counts) >= 0);
    lv_dropdown_set_selected(dd, encoder_index(counts));
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
}

static void settle(void)
{
    uint32_t start = SDL_GetTicks();
    do { lv_timer_handler(); SDL_Delay(5); } while (SDL_GetTicks() - start < 350);
}

static lv_obj_t *find_labeled_parent(lv_obj_t *root, const char *label)
{
    if (lv_obj_check_type(root, &lv_label_class) &&
        strcmp(lv_label_get_text(root), label) == 0) return lv_obj_get_parent(root);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_labeled_parent(lv_obj_get_child(root, i), label);
        if (found) return found;
    }
    return NULL;
}

static unsigned count_dropdowns(lv_obj_t *root)
{
    unsigned count = lv_obj_check_type(root, &lv_dropdown_class) ? 1 : 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++)
        count += count_dropdowns(lv_obj_get_child(root, i));
    return count;
}

static bool check_color_pixels;
static unsigned checked_color_flushes;
static lv_point_t color_points[3];

static void check_color_flush(lv_event_t *e)
{
    if (!check_color_pixels) return;
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_event_get_current_target(e));
    bool swapped = lv_event_get_code(e) == LV_EVENT_FLUSH_START &&
                   bsp_disp_get_color_order() == BSP_COLOR_ORDER_BGR;
    const uint16_t expected[] = { swapped ? 0x001F : 0xF800, 0x07E0, swapped ? 0xF800 : 0x001F };
    for (unsigned i = 0; i < 3; i++) {
        const uint16_t *row = (const uint16_t *)(buf->data + color_points[i].y * buf->header.stride);
        assert(row[color_points[i].x] == expected[i]);
    }
    checked_color_flushes++;
}

static void test_color_order(void)
{
    lv_obj_t *link = find_labeled_parent(lv_screen_active(), TR("屏幕色序"));
    assert(link && lv_obj_check_type(link, &lv_button_class));
    assert(!find_labeled_parent(lv_screen_active(), TR("红"))); /* No samples on parent. */
    lv_obj_send_event(link, LV_EVENT_CLICKED, NULL);
    settle();
    assert(strcmp(panel_mgr_current(), "display_color") == 0);
    assert(count_dropdowns(lv_screen_active()) == 0);
    const char *names[] = { "红", "绿", "蓝" };
    lv_obj_t *choices[3] = {
        find_labeled_parent(lv_screen_active(), TR("默认")),
        find_labeled_parent(lv_screen_active(), "RGB"),
        find_labeled_parent(lv_screen_active(), "BGR"),
    };
    for (unsigned i = 0; i < 3; i++) {
        assert(choices[i] && lv_obj_check_type(choices[i], &lv_button_class));
        lv_obj_t *swatch = find_labeled_parent(lv_screen_active(), TR(names[i]));
        assert(swatch);
        lv_area_t coords;
        lv_obj_get_coords(swatch, &coords);
        color_points[i] = (lv_point_t){coords.x1 + 1, coords.y1 + 1};
    }
    lv_display_add_event_cb(bsp_get_display(), check_color_flush, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(bsp_get_display(), check_color_flush, LV_EVENT_FLUSH_FINISH, NULL);
    check_color_pixels = true;
    for (int mode = 2; mode >= 0; mode--) {
        lv_obj_send_event(choices[mode], LV_EVENT_CLICKED, NULL);
        lv_refr_now(NULL);
        assert(bsp_disp_get_color_order() == mode);
        assert(settings_load_display_color_order() == mode);
        for (int i = 0; i < 3; i++)
            assert(lv_obj_has_state(choices[i], LV_STATE_CHECKED) == (i == mode));
        assert(save_bmp(mode == 2 ? "bgr.bmp" : mode == 1 ? "rgb.bmp" : "color-default.bmp") == 0);
    }
    assert(checked_color_flushes >= 6);
    check_color_pixels = false;
    /* Moving focus alone does not change selection; an actual middle-click does. */
    lv_group_focus_obj(choices[0]);
    bsp_encoder_set_counts_per_detent(4);
    demo_encoder_turn(2);
    settle();
    assert(lv_group_get_focused(ui_nav_active_group()) == choices[1]);
    assert(bsp_disp_get_color_order() == BSP_COLOR_ORDER_DEFAULT);
    demo_encoder_press();
    settle();
    assert(bsp_disp_get_color_order() == BSP_COLOR_ORDER_RGB);
    lv_obj_send_event(choices[0], LV_EVENT_CLICKED, NULL);

    /* Recreate the page in all five languages, also exercising Back/re-entry. */
    for (ui_lang_t language = UI_LANG_ZH; language < UI_LANG_COUNT; language++) {
        panel_mgr_back();
        settle();
        ui_lang_set(language);
        panel_mgr_open("display_color");
        settle();
        assert(count_dropdowns(lv_screen_active()) == 0);
        for (unsigned i = 0; i < 3; i++)
            assert(find_labeled_parent(lv_screen_active(), TR(names[i])));
        if (language == UI_LANG_ZH_TW) {
            /* 字符串存在还不够：缺字占位框也能通过对象查找。 */
            const uint32_t color_glyphs[] = { 0x7D05, 0x7DA0, 0x85CD }; /* 紅綠藍 */
            const uint32_t heading_glyphs[] = { 0x87A2, 0x9810, 0x8A2D }; /* 螢預設 */
            for (unsigned i = 0; i < 3; i++) {
                lv_font_glyph_dsc_t glyph;
                assert(lv_font_get_glyph_dsc(THEME_FONT_S, &glyph, color_glyphs[i], 0) && !glyph.is_placeholder);
                assert(lv_font_get_glyph_dsc(THEME_FONT_M, &glyph, heading_glyphs[i], 0) && !glyph.is_placeholder);
            }
        }
        char path[40];
        snprintf(path, sizeof(path), "colors-%s.bmp", ui_lang_code(language));
        assert(save_bmp(path) == 0);
    }
    ui_lang_set(UI_LANG_ZH);
    panel_mgr_back();
    settle();
    assert(strcmp(panel_mgr_current(), "display") == 0);
    assert(find_labeled_parent(lv_screen_active(), TR("屏幕色序")));
    assert(!bsp_disp_set_color_order(3));
    assert(!settings_save_display_color_order(3));
    puts("PASS: color subpage, three radio choices, five languages, wheel/click, RGB/BGR pixels, persistence and Back");
}

int SDL_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Never touch the user's normal simulator/controller preferences. */
    assert(getenv("KLIPPER_CONFIG_DIR"));
    bsp_init();
    bsp_input_init();
    assert(settings_save_encoder_counts(0));
    assert(settings_save_brightness(75));
    ui_app_create();
    ui_app_open("display");
    settle();
    assert(bsp_encoder_get_counts_per_detent() == 4);
    assert(save_bmp("settings.bmp") == 0);
    assert(lv_dropdown_get_option_count(find_encoder_dropdown(lv_screen_active())) == 3);
    assert(lv_dropdown_get_selected(find_encoder_dropdown(lv_screen_active())) == encoder_index(4));
    if (bsp_disp_can_color_order()) test_color_order();

    choose(2);
    assert(encoder_overlay && bsp_encoder_get_counts_per_detent() == 2);
    assert(settings_load_encoder_counts() == 0); /* Trial is RAM-only. */
    lv_group_t *trial_group = encoder_group;
    lv_obj_t *cancel = lv_group_get_focused(trial_group);
    /* Actual SDL wheel -> ratio conversion -> LVGL focus, not direct callbacks. */
    demo_encoder_turn(1);
    settle();
    assert(lv_group_get_focused(trial_group) != cancel);
    demo_encoder_turn(-1);
    settle();
    assert(lv_group_get_focused(trial_group) == cancel);
    assert(save_bmp("trial.bmp") == 0);
    assert(ui_nav_modal_cancel_top());
    assert(!encoder_overlay && bsp_encoder_get_counts_per_detent() == 4);
    assert(settings_load_encoder_counts() == 0);

    choose(2);
    encoder_started = lv_tick_get() - ENCODER_TRIAL_MS;
    encoder_tick(NULL);
    assert(!encoder_overlay && bsp_encoder_get_counts_per_detent() == 4);
    assert(settings_load_encoder_counts() == 0);

    choose(2);
    lv_group_focus_next(encoder_group);
    lv_obj_send_event(lv_group_get_focused(encoder_group), LV_EVENT_CLICKED, NULL);
    assert(!encoder_overlay && bsp_encoder_get_counts_per_detent() == 2);
    assert(settings_load_encoder_counts() == 2);
    assert(settings_load_brightness() == 75); /* Other preferences preserved. */

    choose(4);
    encoder_started = lv_tick_get() - ENCODER_TRIAL_MS;
    encoder_finish(true); /* Late confirm must still roll back. */
    assert(bsp_encoder_get_counts_per_detent() == 2);
    assert(settings_load_encoder_counts() == 2);

    choose(4);
    panel_mgr_back(); /* Unload must cancel before deleting the focus group. */
    settle();
    assert(!encoder_overlay && !encoder_timer);
    assert(bsp_encoder_get_counts_per_detent() == 2);
    ui_app_open("display");
    settle();
    assert(lv_dropdown_get_selected(find_encoder_dropdown(lv_screen_active())) == encoder_index(2));

    choose(4);
    assert(bsp_encoder_get_counts_per_detent() == 4);
    encoder_finish(true);
    assert(settings_load_encoder_counts() == 4);

    /* Real write failure: nonexistent override dir. Runtime must roll back. */
    char config_dir[1024];
    snprintf(config_dir, sizeof(config_dir), "%s", getenv("KLIPPER_CONFIG_DIR"));
    char missing_dir[1100];
    snprintf(missing_dir, sizeof(missing_dir), "%s/missing", config_dir);
    choose(2);
    _putenv_s("KLIPPER_CONFIG_DIR", missing_dir);
    encoder_finish(true);
    _putenv_s("KLIPPER_CONFIG_DIR", config_dir);
    assert(bsp_encoder_get_counts_per_detent() == 4);
    assert(settings_load_encoder_counts() == 4);

    assert(bsp_conf_write("klipperscreen.conf", "encoder_counts=2oops\n") == 0);
    assert(settings_load_encoder_counts() == 0);
    assert(!settings_save_encoder_counts(9));
    assert(!bsp_encoder_set_counts_per_detent(-1));
    puts("PASS: wheel navigation, trial, cancel, timeout, late confirm, save, default, page unload, write failure, invalid values");
    return 0;
}
