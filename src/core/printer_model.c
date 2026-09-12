/*
 * 真实打印机数据模型（ESP32/Windows）：数据来自平台 Moonraker 客户端，
 * 增量经 lv_async_call 投递到 LVGL 上下文后由 printer_model_apply_status_json 合入，
 * 因此模型读写都在 LVGL 任务里，无需互斥锁。
 *
 * 状态判定对齐 KlipperScreen printer.py evaluate_state 的裁剪版：
 *   klippy(webhooks.state) != ready → DISCONNECTED / ERROR
 *   ready 时看 print_stats.state → STANDBY/PRINTING/PAUSED/COMPLETE
 */
#include "printer.h"
#include "printer_model_internal.h"
#include "moonraker_client.h"
#include "klipper_api.h"
#include "app_settings.h"
#include "bambu_cloud.h"
#include "bambu_monitor.h"
#include "bsp_wifi.h"

#include "cJSON.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct {
    printer_state_t state;
    float ext, bed, ext_t, bed_t;
    float pos[3];
    int   homed[3];
    int   progress;            /* 千分比 */
    char  filename[64];
    float flow;                /* % */
    float speed;               /* % */
    double print_duration;     /* print_stats.print_duration，秒 */
    char  klippy[16];          /* webhooks.state: ready/startup/shutdown/error/... */
    char  print_state[16];     /* print_stats.state: standby/printing/paused/complete/error */
    int   online;              /* Moonraker READY */
    int   rtt_ms;              /* 应用层心跳往返延迟，0=未知 */
    char  gcode_err[96];       /* 待 UI 提示的 klippy 错误（"!!" 行，已去前缀） */
} M = {
    .state = PRINTER_STATE_DISCONNECTED,
    .flow = 100, .speed = 100,
    .klippy = "disconnected",
    .print_state = "standby",
};

static bool klipper_active(void)
{
    return settings_load_machine_mode() == MACHINE_MODE_KLIPPER;
}

static void sync_bambu_device_metadata(const bambu_cloud_snapshot_t *cloud,
                                       bambu_device_conf_t *selected)
{
    if (!cloud || !selected || !selected->valid) return;
    for (int i = 0; i < cloud->device_count; i++) {
        const bambu_cloud_device_t *device = &cloud->devices[i];
        if (strcmp(device->serial, selected->serial) != 0) continue;
        if (strcmp(device->name, selected->name) == 0 &&
            strcmp(device->model, selected->model) == 0) return;
        snprintf(selected->name, sizeof(selected->name), "%s", device->name);
        snprintf(selected->model, sizeof(selected->model), "%s", device->model);
        settings_save_bambu_device(selected);
        return;
    }
}

/* ---- UI 注入的刷新回调（panel_mgr_tick） ---- */
static void (*refresh_hook)(void);
void printer_set_refresh_hook(void (*fn)(void)) { refresh_hook = fn; }
static void refresh(void) { if (refresh_hook) refresh_hook(); }

/* ---------- 读 ---------- */
static bool bambu_status_snapshot(bambu_monitor_snapshot_t *out)
{
    if (settings_load_machine_mode() != MACHINE_MODE_BAMBU ||
        settings_load_bambu_link() != BAMBU_LINK_CLOUD_MONITOR)
        return false;
    bambu_monitor_snapshot(out);
    return out->connected && out->printer.has_data;
}

static printer_state_t bambu_printer_state(void)
{
    bambu_monitor_snapshot_t monitor;
    if (!bambu_status_snapshot(&monitor)) return PRINTER_STATE_DISCONNECTED;
    switch (monitor.printer.state) {
    case BAMBU_PRINT_RUNNING:
    case BAMBU_PRINT_PREPARE:  return PRINTER_STATE_PRINTING;
    case BAMBU_PRINT_PAUSED:   return PRINTER_STATE_PAUSED;
    case BAMBU_PRINT_FINISHED: return PRINTER_STATE_COMPLETE;
    case BAMBU_PRINT_FAILED:   return PRINTER_STATE_ERROR;
    case BAMBU_PRINT_IDLE:
    case BAMBU_PRINT_UNKNOWN:
    default:                   return PRINTER_STATE_STANDBY;
    }
}

printer_state_t printer_state(void)
{
    if (!klipper_active())
        return bambu_printer_state();
    return M.state;
}
printer_capabilities_t printer_capabilities(void)
{
    /* 拓竹后端尚未接入时严禁把操作误发给 Moonraker。 */
    return klipper_active()
           ? PRINTER_CAP_KLIPPER_ALL : 0;
}
float printer_temp_ext(void)   { bambu_monitor_snapshot_t b; return klipper_active() ? M.ext : (bambu_status_snapshot(&b) ? b.printer.nozzle_temp : 0); }
float printer_temp_bed(void)   { bambu_monitor_snapshot_t b; return klipper_active() ? M.bed : (bambu_status_snapshot(&b) ? b.printer.bed_temp : 0); }
float printer_target_ext(void) { bambu_monitor_snapshot_t b; return klipper_active() ? M.ext_t : (bambu_status_snapshot(&b) ? b.printer.nozzle_target : 0); }
float printer_target_bed(void) { bambu_monitor_snapshot_t b; return klipper_active() ? M.bed_t : (bambu_status_snapshot(&b) ? b.printer.bed_target : 0); }
float printer_pos(int axis)    { return klipper_active() ? M.pos[axis] : 0; }
int printer_homed(int axis)    { return klipper_active() ? M.homed[axis] : 0; }
int printer_progress_permille(void) { bambu_monitor_snapshot_t b; return klipper_active() ? M.progress : (bambu_status_snapshot(&b) ? b.printer.progress_percent * 10 : 0); }
const char *printer_filename(void)
{
    static char bambu_name[96];
    if (klipper_active()) return M.filename;
    bambu_monitor_snapshot_t b;
    if (!bambu_status_snapshot(&b)) { bambu_name[0] = 0; return bambu_name; }
    strncpy(bambu_name, b.printer.task_name, sizeof(bambu_name) - 1);
    bambu_name[sizeof(bambu_name) - 1] = 0;
    return bambu_name;
}
float printer_flow_pct(void)   { return klipper_active() ? M.flow : 0; }

/* 取走待提示的 klippy 错误（取后清空）。UI 节拍轮询后弹 toast。 */
bool printer_take_error(char *out, size_t cap)
{
    if (!klipper_active()) return false;
    if (!M.gcode_err[0]) return false;
    strncpy(out, M.gcode_err, cap - 1);
    out[cap - 1] = 0;
    M.gcode_err[0] = 0;
    return true;
}

void printer_model_report_gcode_response(char *msg_heap)
{
    if (strncmp(msg_heap, "!!", 2) == 0) {
        strncpy(M.gcode_err, msg_heap + 2, sizeof(M.gcode_err) - 1);
        M.gcode_err[sizeof(M.gcode_err) - 1] = 0;
        refresh();   /* 立即刷一拍，让 UI 尽快弹出 */
    }
    free(msg_heap);
}

void printer_model_report_rpc_error(char *msg_heap)
{
    /* RPC error 必定是错误（如 "Must home axis first"），直接记 */
    strncpy(M.gcode_err, msg_heap, sizeof(M.gcode_err) - 1);
    M.gcode_err[sizeof(M.gcode_err) - 1] = 0;
    refresh();
    free(msg_heap);
}

uint32_t printer_print_elapsed_s(void)
{
    if (!klipper_active()) {
        bambu_monitor_snapshot_t b;
        if (!bambu_status_snapshot(&b) || b.printer.remaining_minutes < 0 ||
            b.printer.progress_percent <= 0 || b.printer.progress_percent >= 100)
            return 0;
        /* Cloud reports remaining minutes but no elapsed counter. */
        uint64_t remaining = (uint64_t)b.printer.remaining_minutes * 60u;
        return (uint32_t)(remaining * (uint32_t)b.printer.progress_percent /
                          (uint32_t)(100 - b.printer.progress_percent));
    }
    return (M.state == PRINTER_STATE_PRINTING || M.state == PRINTER_STATE_PAUSED)
           ? (uint32_t)M.print_duration : 0;
}

uint32_t printer_print_eta_s(void)
{
    if (!klipper_active()) {
        bambu_monitor_snapshot_t b;
        return bambu_status_snapshot(&b) && b.printer.remaining_minutes >= 0
             ? (uint32_t)b.printer.remaining_minutes * 60u : 0;
    }
    if (M.state != PRINTER_STATE_PRINTING || M.progress < 5) return 0;
    uint32_t el = printer_print_elapsed_s();
    return el * (uint32_t)(1000 - M.progress) / (uint32_t)M.progress;
}

int printer_layer_current(void)
{
    if (klipper_active()) return 0;
    bambu_monitor_snapshot_t b;
    return bambu_status_snapshot(&b) ? b.printer.layer_current : 0;
}

int printer_layer_total(void)
{
    if (klipper_active()) return 0;
    bambu_monitor_snapshot_t b;
    return bambu_status_snapshot(&b) ? b.printer.layer_total : 0;
}

/* ---------- 写（转发 klipper_api；本地状态等订阅回推，不乐观更新） ---------- */
void printer_set_target_ext(float t)
{
    if (!klipper_active()) return;
    char g[48];
    snprintf(g, sizeof(g), "M104 S%d", (int)(t + 0.5f));
    klipper_gcode_script(g);
}

void printer_set_target_bed(float t)
{
    if (!klipper_active()) return;
    char g[48];
    snprintf(g, sizeof(g), "M140 S%d", (int)(t + 0.5f));
    klipper_gcode_script(g);
}

void printer_jog(int axis, float dist)
{
    if (!klipper_active()) return;
    if (axis < 0 || axis > 2) return;
    char g[64];
    snprintf(g, sizeof(g), "G91\nG1 %c%.2f F%d\nG90",
             "XYZ"[axis], (double)dist, axis == 2 ? 600 : 3000);
    klipper_gcode_script(g);
}

void printer_home(int axis)
{
    if (!klipper_active()) return;
    char g[16];
    if (axis < 0) snprintf(g, sizeof(g), "G28");
    else          snprintf(g, sizeof(g), "G28 %c", "XYZ"[axis]);
    klipper_gcode_script(g);
}

void printer_extrude(float mm)
{
    if (!klipper_active()) return;
    char g[48];
    snprintf(g, sizeof(g), "M83\nG1 E%.2f F300", (double)mm);
    klipper_gcode_script(g);
}

void printer_print_start(const char *filename) { if (klipper_active()) klipper_print_start(filename); }
void printer_print_pause(void)  { if (klipper_active()) klipper_print_pause(); }
void printer_print_resume(void) { if (klipper_active()) klipper_print_resume(); }
void printer_print_cancel(void) { if (klipper_active()) klipper_print_cancel(); }
void printer_emergency_stop(void)  { if (klipper_active()) klipper_emergency_stop(); }
void printer_firmware_restart(void){ if (klipper_active()) klipper_firmware_restart(); }

/* ---------- GCode 文件列表 ----------
 * server.files.list {"root":"gcodes"} → moonraker_rpc 应答在 LVGL 上下文
 * 回到这里解析，再转发给面板回调。同时只允许一个在途请求（单面板使用场景）。 */
static struct {
    printer_files_cb cb;
    void *ud;
    bool in_flight;
} files_req;

static void on_files_list(char *json, void *ud)
{
    (void)ud;
    printer_files_cb cb = files_req.cb;
    void *cb_ud = files_req.ud;
    files_req.in_flight = false;

    printer_file_t *files = NULL;
    int count = 0;

    cJSON *arr = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!arr) count = -1;   /* RPC 错误/解析失败：区别于"空列表" */
    if (arr) {
        int n = cJSON_GetArraySize(arr);
        files = n > 0 ? calloc(n, sizeof(printer_file_t)) : NULL;
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!files) break;
            cJSON *path = cJSON_GetObjectItem(it, "path");
            cJSON *size = cJSON_GetObjectItem(it, "size");
            if (!cJSON_IsString(path) || !cJSON_IsNumber(size)) continue;   /* 目录无 size，跳过 */
            if (path->valuestring[0] == '.') continue;                      /* 隐藏文件 */
            printer_file_t *f = &files[count++];
            strncpy(f->name, path->valuestring, sizeof(f->name) - 1);
            f->size = (uint32_t)size->valuedouble;
            cJSON *mod = cJSON_GetObjectItem(it, "modified");
            f->modified = cJSON_IsNumber(mod) ? mod->valuedouble : 0;
        }
        cJSON_Delete(arr);
    }

    if (cb) cb(files, count, cb_ud);
    else    free(files);
}

bool printer_files_refresh(printer_files_cb cb, void *ud)
{
    if (!klipper_active() || !cb || files_req.in_flight ||
        M.state == PRINTER_STATE_DISCONNECTED)
        return false;
    files_req.cb = cb;
    files_req.ud = ud;
    files_req.in_flight = true;
    if (!moonraker_rpc("server.files.list", "{\"root\":\"gcodes\"}", on_files_list, NULL)) {
        files_req.in_flight = false;
        return false;
    }
    return true;
}

void printer_file_delete(const char *name)
{
    if (!klipper_active()) return;
    char path[112];
    snprintf(path, sizeof(path), "gcodes/%s", name);
    klipper_file_delete(path);
}

/* ---------- 状态机 ---------- */
static void evaluate_state(void)
{
    printer_state_t prev = M.state;

    if (!M.online || strcmp(M.klippy, "ready") != 0) {
        if (!M.online || strcmp(M.klippy, "disconnected") == 0 ||
            strcmp(M.klippy, "startup") == 0)
            M.state = PRINTER_STATE_DISCONNECTED;
        else    /* shutdown / error */
            M.state = PRINTER_STATE_ERROR;
    } else if (strcmp(M.print_state, "printing") == 0) {
        M.state = PRINTER_STATE_PRINTING;
    } else if (strcmp(M.print_state, "paused") == 0) {
        M.state = PRINTER_STATE_PAUSED;
    } else if (strcmp(M.print_state, "complete") == 0) {
        M.state = PRINTER_STATE_COMPLETE;
    } else {
        M.state = PRINTER_STATE_STANDBY;
    }

    if (M.state != prev) refresh();   /* 状态跳变立即刷 UI，温度等靠 1s 节拍 */
}

void printer_model_set_online(int online)
{
    M.online = online;
    if (!online) M.rtt_ms = 0;   /* 断线后延迟值失效 */
    evaluate_state();
}

void printer_model_set_rtt(int ms)
{
    M.rtt_ms = ms;   /* 仅存储，UI 节拍自会刷新 */
}

int printer_rtt_ms(void) { return klipper_active() ? M.rtt_ms : 0; }

/* ---------- 增量合入 ---------- */
static float jnum(cJSON *obj, const char *key, float cur)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : cur;
}

static void jstr(cJSON *obj, const char *key, char *out, size_t len)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(out, it->valuestring, len - 1);
        out[len - 1] = 0;
    }
}

void printer_model_apply_status_json(char *json_heap)
{
    cJSON *status = cJSON_Parse(json_heap);
    free(json_heap);
    if (!status) return;

    cJSON *it;
    if ((it = cJSON_GetObjectItem(status, "extruder"))) {
        M.ext   = jnum(it, "temperature", M.ext);
        M.ext_t = jnum(it, "target", M.ext_t);
    }
    if ((it = cJSON_GetObjectItem(status, "heater_bed"))) {
        M.bed   = jnum(it, "temperature", M.bed);
        M.bed_t = jnum(it, "target", M.bed_t);
    }
    if ((it = cJSON_GetObjectItem(status, "toolhead"))) {
        cJSON *pos = cJSON_GetObjectItem(it, "position");
        if (cJSON_IsArray(pos)) {
            for (int i = 0; i < 3; i++) {
                cJSON *v = cJSON_GetArrayItem(pos, i);
                if (cJSON_IsNumber(v)) M.pos[i] = (float)v->valuedouble;
            }
        }
        cJSON *ha = cJSON_GetObjectItem(it, "homed_axes");
        if (cJSON_IsString(ha) && ha->valuestring) {
            const char *s = ha->valuestring;
            M.homed[0] = strchr(s, 'x') != NULL;
            M.homed[1] = strchr(s, 'y') != NULL;
            M.homed[2] = strchr(s, 'z') != NULL;
        }
    }
    if ((it = cJSON_GetObjectItem(status, "print_stats"))) {
        jstr(it, "state", M.print_state, sizeof(M.print_state));
        jstr(it, "filename", M.filename, sizeof(M.filename));
        cJSON *d = cJSON_GetObjectItem(it, "print_duration");
        if (cJSON_IsNumber(d)) M.print_duration = d->valuedouble;
    }
    if ((it = cJSON_GetObjectItem(status, "virtual_sdcard"))) {
        cJSON *p = cJSON_GetObjectItem(it, "progress");
        if (cJSON_IsNumber(p)) M.progress = (int)(p->valuedouble * 1000 + 0.5);
    } else if ((it = cJSON_GetObjectItem(status, "display_status"))) {
        cJSON *p = cJSON_GetObjectItem(it, "progress");
        if (cJSON_IsNumber(p)) M.progress = (int)(p->valuedouble * 1000 + 0.5);
    }
    if ((it = cJSON_GetObjectItem(status, "gcode_move"))) {
        cJSON *f = cJSON_GetObjectItem(it, "extrude_factor");
        if (cJSON_IsNumber(f)) M.flow = (float)(f->valuedouble * 100);
        cJSON *s = cJSON_GetObjectItem(it, "speed_factor");
        if (cJSON_IsNumber(s)) M.speed = (float)(s->valuedouble * 100);
    }
    if ((it = cJSON_GetObjectItem(status, "webhooks"))) {
        jstr(it, "state", M.klippy, sizeof(M.klippy));
    }

    cJSON_Delete(status);
    evaluate_state();
}

/* ---------- 启动编排 ---------- */
static void tick_1s(lv_timer_t *tm)
{
    LV_UNUSED(tm);
    refresh();
}

/* 等 WiFi 连上且 moonraker.conf 就绪后启动客户端（2s 轮询，幂等） */
static void tick_autostart(lv_timer_t *tm)
{
    LV_UNUSED(tm);
    static uint32_t last_bambu_device_refresh;
    if (settings_load_machine_mode() == MACHINE_MODE_BAMBU) {
        moonraker_stop();   /* 幂等：Bambu 模式下 Moonraker 不得持有连接（后端互斥） */
        if (settings_load_bambu_link() == BAMBU_LINK_CLOUD_MONITOR) {
            bambu_cloud_snapshot_t cloud;
            bambu_device_conf_t selected;
            bambu_cloud_snapshot(&cloud);
            if (cloud.state == BAMBU_CLOUD_SIGNED_IN &&
                settings_load_bambu_device(&selected)) {
                if (cloud.device_count > 0) {
                    sync_bambu_device_metadata(&cloud, &selected);
                } else if (!last_bambu_device_refresh ||
                           lv_tick_elaps(last_bambu_device_refresh) >= 60000) {
                    if (bambu_cloud_refresh_devices())
                        last_bambu_device_refresh = lv_tick_get();
                }
                bambu_monitor_start(selected.serial);
            } else {
                bambu_monitor_stop();
            }
        } else {
            bambu_monitor_stop();
        }
        return;
    }
    bambu_monitor_stop();
    moonraker_start();   /* 内部幂等：未配置/WiFi 未连/已在跑都直接返回 */
}

void printer_init(void)
{
    M.online = 0;
    evaluate_state();
    bambu_monitor_init();
    lv_timer_create(tick_1s, 1000, NULL);
    lv_timer_create(tick_autostart, 2000, NULL);
}
