#include "bed_level.h"
#if BSP_HAS_BED_LEVEL
#include "bed_level_config.h"
#include "moonraker_bed_level_esp32.h"
#include "app_settings.h"
#include "printer.h"
#include "moonraker_client.h"
#include "cJSON.h"
#include "lvgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bed_level_data_t data = {.state = BED_LEVEL_LOADING};
static uint32_t generation = 1, request_ms, status_ms, command_ms;
static uint32_t status_token = 1;
#define STATUS_POLL_MS 2000
#define STATUS_RETRY_MS 5000
static bool config_requested, status_pending, command_pending, fresh, homed, unsafe;
static int action = -1; /* >=0 point, -2 calculate, -3 motor off, -1 no command */
static bool after_home;
static float minimum[3], maximum[3], offset[3];
static float current_z;
static moonraker_conf_t endpoint;
static bool allowed(void)
{
    printer_state_t s = printer_state();
    return settings_load_machine_mode() == MACHINE_MODE_KLIPPER &&
           printer_has_capability(PRINTER_CAP_MOVE) &&
           (s == PRINTER_STATE_STANDBY || s == PRINTER_STATE_COMPLETE);
}
static void stop_action(void)
{
    action = -1; command_pending = false; data.busy = false; after_home = false;
}
void bed_level_reset(void)
{
#ifdef ESP_PLATFORM
    moonraker_bed_level_cancel();
#endif
    generation++; status_token++; config_requested = status_pending = fresh = false;
    status_ms = lv_tick_get() - STATUS_POLL_MS;
    stop_action(); memset(&data, 0, sizeof(data)); data.state = BED_LEVEL_LOADING;
}
static bool triple(cJSON *o, const char *key, float *out)
{
    cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return false;
    for (int i = 0; i < 3; i++) {
        cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return false;
        out[i] = (float)v->valuedouble;
        if (!isfinite(out[i])) return false;
    }
    return true;
}
static void request_status(void);
static void execute_action(void);
static bool send_command(const char *script);
static void on_status(char *json, void *ud)
{
    if ((uint32_t)(uintptr_t)ud != status_token) { free(json); return; }
    status_pending = false; fresh = false;
    cJSON *root = cJSON_Parse(json ? json : ""); free(json);
    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *th = cJSON_GetObjectItemCaseSensitive(st, "toolhead");
    cJSON *gm = cJSON_GetObjectItemCaseSensitive(st, "gcode_move");
    float physical[3], logical[3];
    fresh = triple(th, "axis_minimum", minimum) && triple(th, "axis_maximum", maximum) &&
            triple(th, "position", physical) && triple(gm, "gcode_position", logical);
    for (int i = 0; fresh && i < 3; i++) {
        offset[i] = physical[i] - logical[i];
        if (minimum[i] >= maximum[i] || !isfinite(offset[i])) fresh = false;
    }
    if (fresh) current_z = physical[2];
    cJSON *ha = cJSON_GetObjectItemCaseSensitive(th, "homed_axes");
    homed = cJSON_IsString(ha) && strchr(ha->valuestring, 'x') && strchr(ha->valuestring, 'y') && strchr(ha->valuestring, 'z');
    cJSON *idle = cJSON_GetObjectItemCaseSensitive(st, "idle_timeout");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(idle, "state");
    unsafe = cJSON_IsString(state) && !strcmp(state->valuestring, "Printing") && !after_home;
    cJSON *manual = cJSON_GetObjectItemCaseSensitive(st, "bed_screws");
    unsafe |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(manual, "is_active")) || printer_zcal_active();
    cJSON *tilt = cJSON_GetObjectItemCaseSensitive(st, "screws_tilt_adjust");
    cJSON *results = cJSON_GetObjectItemCaseSensitive(tilt, "results");
    for (int i = 0; i < data.count; i++) {
        char key[16]; snprintf(key, sizeof(key), "screw%d", i + 1);
        cJSON *r = cJSON_GetObjectItemCaseSensitive(results, key);
        if (!r) continue;
        cJSON *base = cJSON_GetObjectItemCaseSensitive(r, "is_base");
        cJSON *sign = cJSON_GetObjectItemCaseSensitive(r, "sign");
        cJSON *adjust = cJSON_GetObjectItemCaseSensitive(r, "adjust");
        if (cJSON_IsTrue(base)) snprintf(data.points[i].result, sizeof(data.points[i].result), "base");
        else if (cJSON_IsString(sign) && cJSON_IsString(adjust))
            snprintf(data.points[i].result, sizeof(data.points[i].result), "%.3s %.12s", sign->valuestring, adjust->valuestring);
    }
    cJSON_Delete(root);
    data.status_failed = !fresh;
    if (action != -1) execute_action();
}
static void request_status(void)
{
    char params[384];
    snprintf(params, sizeof(params), "{\"objects\":{\"toolhead\":[\"axis_minimum\",\"axis_maximum\",\"position\",\"homed_axes\"],"
             "\"gcode_move\":[\"gcode_position\"],\"idle_timeout\":[\"state\"],\"%s\":%s%s}}",
             data.probe ? "screws_tilt_adjust" : "bed_screws", data.probe ? "[\"results\",\"error\"]" : "[\"is_active\"]",
             data.probe && data.has_manual ? ",\"bed_screws\":[\"is_active\"]" : "");
    status_ms = lv_tick_get();
    status_pending = moonraker_rpc("printer.objects.query", params, on_status, (void *)(uintptr_t)++status_token);
    if (!status_pending) {
        if (!data.status_failed) fprintf(stderr, "bed_level: status query unavailable%s\n", action == -1 ? " (background)" : " (requested action)");
        fresh = false; data.status_failed = true;
        /* Pending 槽满、断线等后台查询失败不是用户操作失败。保留配置并有界重试。 */
        if (action != -1) { data.operation_failed = true; stop_action(); }
    }
}
static void on_command(char *json, void *ud)
{
    if ((uint32_t)(uintptr_t)ud != generation) { free(json); return; }
    bool ok = json != NULL; free(json); command_pending = false;
    if (!ok) data.operation_failed = true;
    if (!ok && action >= 0 && !after_home && allowed()) {
        /* An intermediate G1 can fail even inside the axis bounding box (e.g.
           delta kinematics). Restore modes without motion; never alter a new
           print which may have started since the request. */
        action = -1;
        if (send_command("RESTORE_GCODE_STATE NAME=KS_BED_LEVEL MOVE=0")) return;
    }
    if (ok && after_home && action != -1 && allowed()) request_status();
    else { stop_action(); request_status(); }
}
static bool send_command(const char *script)
{
    cJSON *params = cJSON_CreateObject();
    if (!params) return false;
    cJSON_AddStringToObject(params, "script", script);
    char *json = cJSON_PrintUnformatted(params); cJSON_Delete(params);
    bool sent = json && moonraker_rpc("printer.gcode.script", json, on_command, (void *)(uintptr_t)generation);
    cJSON_free(json);
    if (!sent) data.operation_failed = true;
    command_pending = sent; command_ms = lv_tick_get(); return sent;
}
static void execute_action(void)
{
    if (!allowed() || !fresh || unsafe || data.state != BED_LEVEL_READY) { data.operation_failed = true; stop_action(); return; }
    if (action == -3) { after_home = false; if (!send_command("M84")) stop_action(); return; }
    if (action >= 0) {
        bed_level_point_t *p = &data.points[action];
        if (p->x < minimum[0] || p->x > maximum[0] || p->y < minimum[1] || p->y > maximum[1] ||
            data.paper_z < minimum[2] || data.travel_z > maximum[2]) { data.operation_failed = true; stop_action(); return; }
    }
    if (!homed) {
        if (after_home) { data.operation_failed = true; stop_action(); return; }
        after_home = true;
        if (!send_command("G28\nM400")) stop_action();
        return;
    }
    after_home = false;
    if (action == -2) {
        /* Result labels from the previous calculation must not masquerade as new. */
        for (int i = 0; i < data.count; i++) data.points[i].result[0] = 0;
        if (!send_command("SCREWS_TILT_CALCULATE\nM400")) stop_action();
        return;
    }
    bed_level_point_t *p = &data.points[action];
    if (p->x < minimum[0] || p->x > maximum[0] || p->y < minimum[1] || p->y > maximum[1] ||
        data.paper_z < minimum[2] || data.travel_z > maximum[2]) { data.operation_failed = true; stop_action(); return; }
    char script[320];
    /* Preserve G90/G91, extrusion mode, feedrate and G92/SET_GCODE_OFFSET.
       Physical minus logical position yields the current coordinate offset. */
    int len = snprintf(script, sizeof(script), "SAVE_GCODE_STATE NAME=KS_BED_LEVEL\nG90\nG1 Z%.3f F%.0f\n"
             "G1 X%.3f Y%.3f F%.0f\nG1 Z%.3f F%.0f\nM400\nRESTORE_GCODE_STATE NAME=KS_BED_LEVEL MOVE=0",
             fmaxf(data.travel_z, current_z)-offset[2], data.lift_speed*60, p->x-offset[0], p->y-offset[1], data.speed*60,
             data.paper_z-offset[2], data.lift_speed*60);
    if (len <= 0 || len >= (int)sizeof(script) || !send_command(script)) {
        data.operation_failed = true; stop_action();
    }
}
#ifndef ESP_PLATFORM
static void on_config(char *json, void *ud)
{
    if ((uint32_t)(uintptr_t)ud != generation) { free(json); return; }
    bed_level_config_stream_t *s = calloc(1, sizeof(*s));
    if (!s) { free(json); data.state = BED_LEVEL_FAILED; return; }
    bed_level_config_begin(s);
    if (!json || !bed_level_config_feed(s, json, strlen(json)) || !bed_level_config_finish(s, &data)) data.state = BED_LEVEL_FAILED;
    free(json); free(s);
}
#endif
const bed_level_data_t *bed_level_poll(void)
{
    moonraker_conf_t conf;
    bool online = allowed() || printer_state() == PRINTER_STATE_PRINTING || printer_state() == PRINTER_STATE_PAUSED;
    if (!online || settings_load_machine_mode() != MACHINE_MODE_KLIPPER) { data.can_move = false; return &data; }
    if (settings_load_moonraker(&conf) && (strcmp(endpoint.host, conf.host) || endpoint.port != conf.port || strcmp(endpoint.api_key, conf.api_key))) {
        bed_level_reset(); endpoint = conf;
    }
#ifdef KLIPPER_DESKTOP_SIMULATOR
    if (!config_requested) {
        const char *n = getenv("KLIPPER_BED_POINTS");
        data.count = n ? atoi(n) : 4; data.probe = true;
        data.state = data.count == 3 || data.count == 4 ? BED_LEVEL_READY : BED_LEVEL_MISSING;
        for (int i = 0; i < data.count && i < 4; i++) {
            data.points[i].x = data.count == 3 && i == 2 ? 110 : i % 2 ? 200 : 20;
            data.points[i].y = i < 2 ? 20 : 200;
            const char *names[] = {"Front left", "Front right", data.count == 3 ? "Rear center" : "Rear left", "Rear right"};
            snprintf(data.points[i].name, sizeof(data.points[i].name), "%s", names[i]);
            /* Opt-in screenshot fixture only; never compiled into a real printer backend. */
            if (getenv("KLIPPER_BED_RESULTS")) {
                const char *values[] = {"base", "CW 00:12", "CCW 00:08", "CW 01:02"};
                snprintf(data.points[i].result, sizeof(data.points[i].result), "%s", values[i]);
            }
        }
        config_requested = true;
    }
    data.can_move = allowed(); return &data;
#else
    if (!config_requested) {
#ifdef ESP_PLATFORM
        config_requested = moonraker_bed_level_start();
#else
        config_requested = moonraker_rpc("printer.objects.query", "{\"objects\":{\"configfile\":[\"config\"]}}", on_config, (void *)(uintptr_t)generation);
#endif
        request_ms = lv_tick_get();
        if (!config_requested) data.state = BED_LEVEL_FAILED;
    }
#ifdef ESP_PLATFORM
    moonraker_bed_level_poll(&data);
#endif
    if (data.state == BED_LEVEL_LOADING && config_requested && lv_tick_elaps(request_ms) > 20000) bed_level_reset();
    if (command_pending && lv_tick_elaps(command_ms) > 180000) { bed_level_reset(); data.operation_failed = true; return &data; }
    if (status_pending && lv_tick_elaps(status_ms) > 10000) {
        status_pending = false; status_token++; fresh = false; data.status_failed = true;
        status_ms = lv_tick_get();
        if (action != -1) { data.operation_failed = true; stop_action(); }
    }
    if (data.state == BED_LEVEL_READY && !status_pending && !command_pending &&
        lv_tick_elaps(status_ms) >= (data.status_failed ? STATUS_RETRY_MS : STATUS_POLL_MS)) request_status();
    data.can_move = allowed() && fresh && !unsafe && !data.busy && !command_pending;
    return &data;
#endif
}
static bool start_action(int which)
{
    if (!allowed() || data.state != BED_LEVEL_READY || data.busy || command_pending) return false;
#ifdef KLIPPER_DESKTOP_SIMULATOR
    (void)which; return true;
#else
    action = which; data.busy = true; data.operation_failed = false; after_home = false;
    if (!status_pending) request_status();
    return status_pending;
#endif
}
bool bed_level_move(int point) { return point >= 0 && point < data.count && start_action(point); }
bool bed_level_calculate(void) { return data.probe && start_action(-2); }
bool bed_level_motors_off(void) { return start_action(-3); }
#endif
