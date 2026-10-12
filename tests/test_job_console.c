/* Real UI/model/widget, deterministic offline HTTP/RPC delivery. No printer writes.
 * Windows desktop CMake: -DKR_JOB_CONSOLE_TEST=ON, target test_job_console.
 * Run with KLIPPER_CONFIG_DIR pointing at an empty temporary directory. */
#include <SDL.h>
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp.h"
#include "bsp_caps.h"
#include "ui_buttons.h"
#include "titlebar.h"
#include "gcode_thumbnail.h"
#include "moonraker_client.h"

static void (*history_reply)(char *, void *);
static void *history_ud;
static unsigned history_requests, refreshes;
static bool rpc_ok = true;
static bool fake_rpc(const char *method, const char *params, void (*cb)(char *, void *), void *ud)
{
    assert(!strcmp(method, "server.gcode_store")); (void)params;
    history_requests++; history_reply = cb; history_ud = ud;
    return rpc_ok;
}
#define moonraker_rpc fake_rpc
#include "../src/core/printer_model.c"
#undef moonraker_rpc

static struct {
    void (*cb)(unsigned char *, size_t, void *);
    void *ud;
    char name[96];
} fetches[32];
static unsigned fetch_count;
static bool fetch_ok = true;
static bool fake_fetch(const char *name, void (*cb)(unsigned char *, size_t, void *), void *ud)
{
    if (!fetch_ok) return false;
    assert(fetch_count < 32);
    fetches[fetch_count].cb = cb; fetches[fetch_count].ud = ud;
    snprintf(fetches[fetch_count].name, sizeof(fetches[0].name), "%s", name);
    fetch_count++;
    return true;
}
/* Exercise the actual remote widget on Windows, including DELETE/delivery
 * ownership, without enabling thumbnails in the shipped Windows build. */
#undef BSP_HAS_GCODE_THUMB
#define BSP_HAS_GCODE_THUMB 1
#define gcode_thumbnail_fetch_async fake_fetch
#include "../src/ui/widgets/gcode_thumb.c"
#undef gcode_thumbnail_fetch_async
#include "../src/ui/panels/panel_job_status.c"
#include "../src/ui/panels/panel_console.c"

static void count_refresh(void) { refreshes++; }
static void settle(void)
{
    uint32_t start = SDL_GetTicks();
    do { lv_timer_handler(); SDL_Delay(5); } while (SDL_GetTicks() - start < 300);
}

static void status(const char *state, const char *name, unsigned elapsed)
{
    char json[512];
    snprintf(json, sizeof(json), "{\"webhooks\":{\"state\":\"ready\"},"
        "\"print_stats\":{\"state\":\"%s\",\"filename\":\"%s\",\"print_duration\":%u}}",
        state, name, elapsed);
    printer_model_apply_status_json(strdup(json));
    panel_mgr_tick();
}

static void deliver(unsigned i, bool success)
{
    /* Valid 1x1 RGBA PNG. These tests assert widget ownership/status, not pixels. */
    static const unsigned char png[] = {
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,
        31,21,196,137,0,0,0,11,73,68,65,84,120,156,99,0,2,0,0,5,0,1,233,250,220,
        216,0,0,0,0,73,69,78,68,174,66,96,130
    };
    unsigned char *copy = NULL;
    if (success) { copy = malloc(sizeof(png)); assert(copy); memcpy(copy, png, sizeof(png)); }
    fetches[i].cb(copy, success ? sizeof(png) : 0, fetches[i].ud);
}

static void deliver_history(void)
{
    history_reply(strdup("{\"gcode_store\":[{\"type\":\"command\",\"message\":\"M115\"},"
        "{\"type\":\"response\",\"message\":\"ok\"}]}"), history_ud);
}

static lv_obj_t *find_button(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))
        return lv_obj_get_parent(root);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_button(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

int SDL_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    assert(getenv("KLIPPER_CONFIG_DIR"));
    bsp_init(); bsp_input_init(); ui_layout_init(); ui_lang_set(UI_LANG_ZH); theme_load();
    ui_nav_init(); ui_buttons_init(); titlebar_init(); panel_mgr_init();
    printer_set_refresh_hook(count_refresh);
    moonraker_conf_t conf = {.host = "offline-test", .port = 7125, .valid = true};
    settings_save_machine_mode(MACHINE_MODE_KLIPPER); settings_save_moonraker(&conf);
    printer_model_set_online(1);

    status("printing", "", 0); panel_mgr_open("job_status"); settle();
    assert(job_thumb == NULL && fetch_count == 0); /* Boot before filename arrives. */
    status("printing", "first.gcode", 1);
    assert(fetch_count == 1 && !strcmp(fetches[0].name, "first.gcode"));
    deliver(0, false); assert(gcode_thumb_state(job_thumb) == GCODE_THUMB_FAILED);
    thumb_attempt_at = lv_tick_get() - 3001; panel_mgr_tick(); assert(fetch_count == 2);
    status("printing", "second.gcode", 0);
    lv_obj_t *second = job_thumb;
    assert(fetch_count == 3 && !strcmp(fetches[2].name, "second.gcode"));
    deliver(1, true); /* Old task's late reply must not replace task two. */
    assert(job_thumb == second && gcode_thumb_state(second) == GCODE_THUMB_LOADING);
    deliver(2, true); assert(gcode_thumb_state(second) == GCODE_THUMB_READY);
    for (int i = 0; i < 5; i++) panel_mgr_tick(); assert(fetch_count == 3);
    status("complete", "second.gcode", 10); status("printing", "second.gcode", 0);
    assert(fetch_count == 4); /* Same filename, next job. */
    status("printing", "", 0); assert(job_thumb == NULL);
    deliver(3, true); /* Safe when the next job's filename is still unknown. */
    status("printing", "third.gcode", 0); assert(fetch_count == 5);
    deliver(4, true);
    status("printing", "missing.gcode", 0); assert(fetch_count == 6); deliver(5, false);
    for (int i = 0; i < 3; i++) {
        thumb_attempt_at = lv_tick_get() - 13000; panel_mgr_tick();
        assert(fetch_count == (unsigned)(7 + i)); deliver(fetch_count - 1, false);
    }
    thumb_attempt_at = lv_tick_get() - 13000;
    for (int i = 0; i < 5; i++) panel_mgr_tick(); assert(fetch_count == 9);
    status("printing", "fourth.gcode", 0); assert(fetch_count == 10);
    panel_mgr_open("console"); settle(); deliver(9, true); /* Safe after screen deletion. */

    assert(history_requests == 1);
    unsigned before = refreshes; deliver_history(); assert(refreshes == before + 1);
    panel_mgr_tick(); assert(printer_console_line_count() == 2);
    lv_obj_send_event(find_button(lv_screen_active(), "清除"), LV_EVENT_CLICKED, NULL);
    assert(printer_console_line_count() == 0 && lv_obj_get_child_count(list) == 0);
    assert(!strcmp(lv_label_get_text(lbl_last), TR("暂无回显")));
    panel_mgr_back(); settle(); panel_mgr_open("console"); settle();
    assert(history_requests == 1 && printer_console_line_count() == 0);

    snprintf(conf.host, sizeof(conf.host), "%s", "other-offline-test");
    settings_save_moonraker(&conf); printer_console_load_history(); assert(history_requests == 2);
    printer_console_load_history(); assert(history_requests == 2); /* No duplicate in-flight fetch. */
    void (*late)(char *, void *) = history_reply; void *late_ud = history_ud;
    printer_console_clear();
    printer_model_report_gcode_response(strdup("// new live output"));
    late(strdup("{\"gcode_store\":[{\"message\":\"old output\"}]}"), late_ud);
    assert(printer_console_line_count() == 1 && strstr(printer_console_line(0, NULL), "new live"));
    panel_mgr_back(); settle(); panel_mgr_open("console"); settle(); assert(history_requests == 2);
    printer_model_set_online(0); printer_model_set_online(1); printer_console_load_history();
    assert(history_requests == 2); /* Reconnect must not undo a clear. */

    snprintf(conf.host, sizeof(conf.host), "%s", "third-offline-test");
    settings_save_moonraker(&conf); rpc_ok = false; printer_console_load_history();
    assert(!M_con_history_pending); rpc_ok = true; printer_console_load_history();
    printer_model_report_gcode_response(strdup("live while history pending")); deliver_history();
    assert(printer_console_line_count() == 1 && strstr(printer_console_line(0, NULL), "live while"));
    puts("PASS: late boot filename, file/same-file task switch, bounded retry, stale/deleted image deliveries");
    puts("PASS: immediate console clear, reopen/reconnect, stale history rejection, endpoint switch, live output");
    return 0;
}
