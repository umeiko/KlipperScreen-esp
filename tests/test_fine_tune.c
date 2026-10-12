/* Real model + real UI, fake only the command transport. Never connect to a printer.
 * Build: desktop CMake -DKR_FINE_TUNE_TEST=ON, target test_fine_tune.
 * Run with KLIPPER_CONFIG_DIR pointing at an empty temporary directory. */
#include <SDL.h>
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp.h"
#include "printer.h"
#include "ui_buttons.h"
#include "titlebar.h"
#include "app_settings.h"

static char last_command[100];
static unsigned sent_count;
static bool transport_ok = true;
static bool fake_gcode(const char *cmd)
{
    if (!transport_ok) return false;
    snprintf(last_command, sizeof(last_command), "%s", cmd);
    sent_count++;
    return true;
}
#define klipper_gcode_script fake_gcode
#include "../src/core/printer_model.c"
#undef klipper_gcode_script
#include "../src/ui/panels/panel_fine_tune.c"

static void settle(void)
{
    uint32_t start = SDL_GetTicks();
    do { lv_timer_handler(); SDL_Delay(5); } while (SDL_GetTicks() - start < 350);
    lv_obj_update_layout(lv_screen_active());
}

static void status(const char *state, float speed, float flow, float z)
{
    char json[512];
    snprintf(json, sizeof(json),
        "{\"webhooks\":{\"state\":\"ready\"},\"print_stats\":{\"state\":\"%s\"},"
        "\"toolhead\":{\"homed_axes\":\"xyz\"},\"gcode_move\":{\"speed_factor\":%.4f,"
        "\"extrude_factor\":%.4f,\"homing_origin\":[0,0,%.4f]}}", state, speed / 100, flow / 100, z);
    printer_model_apply_status_json(strdup(json));
}

static void click(int kind, int action)
{
    lv_obj_send_event(actions[kind][action], LV_EVENT_CLICKED, NULL);
}

static void check_layout(void)
{
    for (int k = 0; k < TUNE_COUNT; k++) {
        assert(lv_text_get_width(lv_label_get_text(values[k]),
            strlen(lv_label_get_text(values[k])), lv_obj_get_style_text_font(values[k], 0), 0)
            <= lv_obj_get_width(values[k]));
        for (int a = 0; a < 3; a++) {
            lv_area_t box; lv_obj_get_coords(actions[k][a], &box);
            assert(box.x1 >= 0 && box.x2 < ui_scr_w());
            assert(box.y1 >= THEME_TITLEBAR_H && box.y2 < ui_scr_h());
        }
        for (int i = 0; k < 2 && i < (k == TUNE_Z ? 2 : 4); i++) {
            lv_obj_t *lbl = lv_obj_get_child(steps[k][i], 0);
            assert(lv_text_get_width(lv_label_get_text(lbl), strlen(lv_label_get_text(lbl)),
                lv_obj_get_style_text_font(lbl, 0), 0) <= lv_obj_get_content_width(steps[k][i]));
        }
    }
}

int SDL_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    assert(getenv("KLIPPER_CONFIG_DIR"));
    bsp_init(); bsp_input_init(); ui_layout_init(); theme_load();
    ui_nav_init(); ui_buttons_init(); titlebar_init(); panel_mgr_init();
    settings_save_machine_mode(MACHINE_MODE_KLIPPER);
    printer_model_set_online(1);
    status("standby", 100, 100, 0);
    panel_mgr_open("fine_tune"); assert(strcmp(panel_mgr_current(), "main") == 0);
    assert(!printer_set_speed_pct(105));
    status("printing", 100, 100, 0);
    panel_mgr_open("job_status"); settle();
    assert(!lv_obj_has_flag(titlebar_motoroff_button(), LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(titlebar_motoroff_button(), LV_EVENT_CLICKED, NULL); settle();
    assert(strcmp(panel_mgr_current(), "fine_tune") == 0);
    assert(lv_obj_has_flag(titlebar_motoroff_button(), LV_OBJ_FLAG_HIDDEN));
    check_layout();

    click(TUNE_SPEED, 1); assert(!strcmp(last_command, "M220 S105"));
    click(TUNE_SPEED, 1); assert(!strcmp(last_command, "M220 S110"));
    status("printing", 105, 100, 0); update_ui(); assert(shown[TUNE_SPEED] == 110);
    status("printing", 110, 100, 0); update_ui(); assert(!pending[TUNE_SPEED]);
    status("printing", 120, 100, 0); update_ui(); assert(shown[TUNE_SPEED] == 120);
    click(TUNE_SPEED, 0); assert(!strcmp(last_command, "M220 S100"));
    status("printing", 100, 100, 0); update_ui();
    click(TUNE_FLOW, 2); assert(!strcmp(last_command, "M221 S95"));
    click(TUNE_FLOW, 0); assert(!strcmp(last_command, "M221 S100"));
    lv_obj_send_event(steps[1][2], LV_EVENT_CLICKED, NULL); /* Shared 10% step. */
    click(TUNE_SPEED, 1); assert(!strcmp(last_command, "M220 S110"));
    click(TUNE_FLOW, 1); assert(!strcmp(last_command, "M221 S110"));
    status("printing", 110, 110, 0); update_ui();
    lv_obj_send_event(steps[1][1], LV_EVENT_CLICKED, NULL);
    click(TUNE_Z, 2); assert(strstr(last_command, "Z_ADJUST=-0.010"));
    update_ui(); assert(!strcmp(lv_label_get_text(values[TUNE_Z]), "-0.01mm"));
    click(TUNE_Z, 0); assert(strstr(last_command, "Z=0 MOVE=1"));
    M.homed[2] = 0; update_ui();
    unsigned before = sent_count; click(TUNE_Z, 1); assert(before == sent_count);
    M.homed[2] = 1;
    assert(!printer_set_speed_pct(0) && !printer_set_flow_pct(1001));
    assert(!printer_adjust_z_offset(0.5f) && !printer_adjust_z_offset(NAN));

    status("paused", 100, 100, 0); update_ui();
    click(TUNE_SPEED, 2); assert(!strcmp(last_command, "M220 S95"));
    transport_ok = false; before = sent_count; click(TUNE_SPEED, 1); assert(sent_count == before);
    transport_ok = true; sent_at[TUNE_SPEED] = lv_tick_get() - 3001; update_ui();
    assert(shown[TUNE_SPEED] == 100);
    status("complete", 100, 100, 0); before = sent_count; click(TUNE_SPEED, 1);
    assert(sent_count == before); settle(); assert(!strcmp(panel_mgr_current(), "job_status"));
    panel_mgr_tick(); assert(lv_obj_has_flag(titlebar_motoroff_button(), LV_OBJ_FLAG_HIDDEN));

    for (int l = 0; l < UI_LANG_COUNT; l++) {
        ui_lang_set(l); status("printing", 1000, 1000, -0.05f);
        panel_mgr_open("fine_tune"); settle(); check_layout();
        click(TUNE_SPEED, 1); assert(!strcmp(last_command, "M220 S1000"));
        panel_mgr_back(); settle();
    }
    status("printing", 100, 100, 0);
    panel_mgr_open("fine_tune"); settle();
    printer_model_set_online(0); before = sent_count; click(TUNE_FLOW, 1);
    assert(before == sent_count); settle(); assert(!strcmp(panel_mgr_current(), "job_status"));
    settings_save_machine_mode(MACHINE_MODE_BAMBU);
    assert(!printer_can_fine_tune());
    panel_mgr_open("fine_tune"); assert(!strcmp(panel_mgr_current(), "job_status"));
    puts("PASS: task/capability gating, command strings, rapid taps, status echo, resets, pause,");
    puts("transport rejection, limits, late click after completion/disconnect, five languages/reopen/layout");
    return 0;
}
