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
#include "file_list_stream.h"
#ifdef ESP_PLATFORM
#include "moonraker_files_esp32.h"
#endif

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

/* 菜单页扩展能力的内部前置声明（实现在文件后段） */
static void console_append(const char *text, int kind);
static bool console_is_temp_line(const char *s);

#define MACRO_MAX 24
#define FAN_MAX 8
#define FILS_MAX 4
#define CONSOLE_MAX_LINES 80
#define CONSOLE_LINE_MAX 96

/* 菜单页扩展对象清单/状态（实现在文件后段；tentative 定义，初始化由语义字段把关） */
typedef struct {
    char name[48];    /* 原始对象名（gcode 用）：宏名 / "fan_generic xxx" / 传感器名 */
    char label[48];   /* 显示名：宏去前缀下划线转空格；风扇/传感器用短名 */
    bool writable;    /* 风扇用：fan/fan_generic=1，heater/controller_fan=0 */
    float speed;      /* 风扇 0..1，-1 未知 */
    bool enabled;     /* 断料 */
    bool detected;    /* 断料 */
} menu_obj_t;

static menu_obj_t M_macros[MACRO_MAX];  static int M_macro_cnt;
static menu_obj_t M_fans[FAN_MAX];      static int M_fan_cnt;
static menu_obj_t M_fils[FILS_MAX];     static int M_fil_cnt;
static bool M_probe_present;                /* objects.list 有 probe/bltouch 等 */
static float M_probe_z_offset;              /* probe/bltouch 订阅的 z_offset */
static bool  M_probe_z_valid;
static float M_home_origin_z;               /* gcode_move.homing_origin[2] */
static bool  M_home_origin_valid;
static int  M_endstop[3];
static uint32_t M_endstop_ms;
static bool M_endstop_fresh;            /* 已收到过至少一次 QUERY_ENDSTOPS 结果 */
static bool M_manual_probe_active;

static struct { char text[CONSOLE_LINE_MAX]; uint8_t kind; } M_con[CONSOLE_MAX_LINES];
static int M_con_head, M_con_cnt;       /* head=最旧 */

/* gcode.help 的校准命令可用性（连接后按存在性建） */
static char M_zcal_cmds[2][32];
static int  M_zcal_cnt;
static bool M_zcal_loaded;
static bool M_zcal_loading;   /* gcode.help 在途（失败下一拍重试，避免每拍重发） */

void printer_model_report_gcode_response(char *msg_heap)
{
    /* 先按行解析限位回流（QUERY_ENDSTOPS："x:open" / "z:TRIGGERED"，可单行可多行），
     * 喂给传感器页；行本身照常进控制台——刷新已改为手动触发，用户
     * 自己发的 M119/QUERY_ENDSTOPS 理应看到回显（自动轮询时代才需要过滤） */
    for (const char *p = msg_heap; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t ln = eol ? (size_t)(eol - p) : strlen(p);
        if (ln >= 3 && ln < 40 && p[1] == ':' &&
            (p[0] == 'x' || p[0] == 'y' || p[0] == 'z')) {
            int axis = p[0] - 'x';
            M_endstop[axis] = strncmp(p + 2, "TRIGGERED", 9) == 0 ? 1 : 0;
            M_endstop_ms = lv_tick_get();
            M_endstop_fresh = true;
        }
        if (!eol) break;
        p = eol + 1;
    }
    /* 温度轮询行（ok B:.. T0:..）不进控制台，避免刷屏 */
    if (!console_is_temp_line(msg_heap)) {
        int kind = 0;
        const char *text = msg_heap;
        if (strncmp(msg_heap, "!!", 2) == 0)      { kind = 2; text = msg_heap + 2; }
        else if (strncmp(msg_heap, "//", 2) == 0) { kind = 3; text = msg_heap + 2; }
        console_append(text, kind);
    }
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
    if (axis < 0)       snprintf(g, sizeof(g), "G28");
    else if (axis == 3) snprintf(g, sizeof(g), "G28 X Y");
    else                snprintf(g, sizeof(g), "G28 %c", "XYZ"[axis]);
    klipper_gcode_script(g);
}

void printer_motors_off(void)
{
    if (!klipper_active()) return;
    klipper_gcode_script("M84");
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

/* Bounded GCode pages: ESP32 uses an independent streaming HTTP worker.
 * Desktop transports retain RPC but parse its result one record at a time.
 * One active request; page lifetime is limited to the callback, stale replies
 * are rejected by generation, and timeout is polled without status traffic. */
#define FILES_REQ_TIMEOUT_MS 25000
static struct {
    printer_files_cb cb;
    void *ud;
    bool in_flight;
    uint32_t start_ms;
    uintptr_t generation;
    unsigned offset;
} files_req;

/* 在途请求失败收尾：清标记并回调失败（count=-1）。LVGL 上下文调用。 */
static void files_req_fail(void)
{
    printer_files_cb cb = files_req.cb;
    void *ud = files_req.ud;
    printer_files_cancel();
    if (cb) cb(NULL, ud);
}

#ifndef ESP_PLATFORM
static void on_files_list(char *json, void *ud)
{
    if (!files_req.in_flight || (uintptr_t)ud != files_req.generation) { free(json); return; }
    printer_files_cb cb = files_req.cb;
    void *cb_ud = files_req.ud;
    files_req.in_flight = false;

    file_list_stream_t stream;
    file_list_stream_init(&stream, files_req.offset);
    bool ok = json && file_list_stream_feed(&stream, json, strlen(json)) && file_list_stream_finish(&stream);
    free(json);
    if (cb) cb(ok ? &stream.page : NULL, cb_ud);
}
#endif

bool printer_files_refresh(unsigned offset, printer_files_cb cb, void *ud)
{
    if (!klipper_active() || !cb || files_req.in_flight ||
        M.state == PRINTER_STATE_DISCONNECTED)
        return false;
    files_req.cb = cb;
    files_req.ud = ud;
    files_req.in_flight = true;
    files_req.start_ms = lv_tick_get();
    files_req.offset = offset;
    files_req.generation++;
#ifdef ESP_PLATFORM
    bool started = moonraker_files_start(offset);
#else
    bool started = moonraker_rpc("server.files.list", "{\"root\":\"gcodes\"}", on_files_list,
                                 (void *)files_req.generation);
#endif
    if (!started) {
        files_req.in_flight = false;
        return false;
    }
    return true;
}

void printer_files_cancel(void)
{
    files_req.in_flight = false;
    files_req.cb = NULL;
    files_req.generation++;
#ifdef ESP_PLATFORM
    moonraker_files_cancel();
#endif
}

void printer_files_poll(void)
{
    if (!files_req.in_flight) return;
#ifdef ESP_PLATFORM
    printer_file_page_t page;
    if (moonraker_files_poll(&page)) {
        printer_files_cb cb = files_req.cb;
        void *ud = files_req.ud;
        files_req.in_flight = false;
        if (cb) cb(page.count < 0 ? NULL : &page, ud);
        return;
    }
#endif
    if (lv_tick_elaps(files_req.start_ms) > FILES_REQ_TIMEOUT_MS) files_req_fail();
}

void printer_file_delete(const char *name)
{
    if (!klipper_active()) return;
    char path[112];
    snprintf(path, sizeof(path), "gcodes/%s", name);
    klipper_file_delete(path);
}

/* ---------- 菜单页扩展能力（宏/风扇/断料/限位/控制台/Z 校准） ---------- */
/* 存储声明在文件头部（report_gcode_response 要用 endstop 状态），此处只有实现 */

static void on_gcode_store(char *result_json, void *ud);
static void on_gcode_help(char *result_json, void *ud);

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void labelize(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i] == '_' ? ' ' : src[i];
    dst[i] = 0;
}

/* objects.list 结果数组 → 宏/风扇/断料清单。LVGL 上下文。
 * 容错：拿到的是外层包裹 {"objects":[...]} 时先解包。 */
void printer_model_set_object_names(char *json_heap)
{
    cJSON *arr = cJSON_Parse(json_heap);
    free(json_heap);
    if (arr && !cJSON_IsArray(arr)) {
        cJSON *inner = cJSON_GetObjectItem(arr, "objects");
        if (cJSON_IsArray(inner)) {
            cJSON *d = cJSON_Duplicate(inner, 1);
            cJSON_Delete(arr);
            arr = d;
        }
    }
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }

    M_macro_cnt = M_fan_cnt = M_fil_cnt = 0;
    M_probe_present = false;
    M_probe_z_valid = false;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring) continue;
        const char *obj = it->valuestring;
        if (strncmp(obj, "gcode_macro ", 12) == 0) {
            const char *name = obj + 12;
            if (name[0] == '_') continue;                    /* 内部宏不显示 */
            if (strcmp(name, "LOAD_FILAMENT") == 0 ||
                strcmp(name, "UNLOAD_FILAMENT") == 0) continue; /* 挤出页已有装料/退料 */
            if (M_macro_cnt < MACRO_MAX) {
                menu_obj_t *m = &M_macros[M_macro_cnt++];
                copy_text(m->name, sizeof(m->name), name);
                labelize(m->label, sizeof(m->label), name);
                m->writable = true;
            }
        } else if (strcmp(obj, "fan") == 0 ||
                   strncmp(obj, "fan_generic ", 12) == 0 ||
                   strncmp(obj, "heater_fan ", 11) == 0 ||
                   strncmp(obj, "controller_fan ", 15) == 0) {
            if (M_fan_cnt < FAN_MAX) {
                menu_obj_t *f = &M_fans[M_fan_cnt++];
                memset(f, 0, sizeof(*f));
                copy_text(f->name, sizeof(f->name), obj);
                const char *short_name = strrchr(obj, ' ');
                copy_text(f->label, sizeof(f->label), short_name ? short_name + 1 : obj);
                f->writable = strcmp(obj, "fan") == 0 ||
                              strncmp(obj, "fan_generic ", 12) == 0;
                f->speed = -1;
            }
        } else if (strncmp(obj, "filament_switch_sensor ", 23) == 0 ||
                   strncmp(obj, "filament_motion_sensor ", 23) == 0) {
            if (M_fil_cnt < FILS_MAX) {
                menu_obj_t *s = &M_fils[M_fil_cnt++];
                memset(s, 0, sizeof(*s));
                copy_text(s->name, sizeof(s->name), obj);
                const char *short_name = strrchr(obj, ' ');
                /* 名字同时是 gcode 参数（SET_FILAMENT_SENSOR SENSOR=<label>），
                   不能用 labelize——那会把下划线换成空格，Klipper 直接查无此传感器 */
                copy_text(s->label, sizeof(s->label), short_name ? short_name + 1 : obj);
            }
        } else if (strcmp(obj, "probe") == 0 || strcmp(obj, "bltouch") == 0 ||
                   strcmp(obj, "smart_effector") == 0 ||
                   strcmp(obj, "probe_eddy_current") == 0) {
            M_probe_present = true;   /* Z 校准页显示已存偏移用 */
        }
    }
    cJSON_Delete(arr);
    refresh();
}

int printer_macro_count(void) { return klipper_active() ? M_macro_cnt : 0; }
const char *printer_macro_name(int i)  { return (i >= 0 && i < M_macro_cnt) ? M_macros[i].name : ""; }
const char *printer_macro_label(int i) { return (i >= 0 && i < M_macro_cnt) ? M_macros[i].label : ""; }
void printer_macro_run(int i)
{
    if (!klipper_active() || i < 0 || i >= M_macro_cnt) return;
    klipper_gcode_script(M_macros[i].name);
}

int printer_fan_count(void) { return klipper_active() ? M_fan_cnt : 0; }
const char *printer_fan_name(int i) { return (i >= 0 && i < M_fan_cnt) ? M_fans[i].label : ""; }
float printer_fan_speed(int i)      { return (i >= 0 && i < M_fan_cnt) ? M_fans[i].speed : -1; }
bool printer_fan_writable(int i)    { return (i >= 0 && i < M_fan_cnt) ? M_fans[i].writable : false; }
void printer_fan_set(int i, float speed)
{
    if (!klipper_active() || i < 0 || i >= M_fan_cnt || !M_fans[i].writable) return;
    if (speed < 0) speed = 0;
    if (speed > 1) speed = 1;
    char g[96];   /* SET_FAN_SPEED FAN=<label> SPEED=x.xx 最坏 ~77B，64 放不下（-Werror=format-truncation） */
    if (strcmp(M_fans[i].name, "fan") == 0)
        snprintf(g, sizeof(g), "M106 S%d", (int)(speed * 255 + 0.5f));
    else
        snprintf(g, sizeof(g), "SET_FAN_SPEED FAN=%s SPEED=%.2f",
                 M_fans[i].label, (double)speed);
    klipper_gcode_script(g);
}

int printer_filsensor_count(void) { return klipper_active() ? M_fil_cnt : 0; }
const char *printer_filsensor_name(int i) { return (i >= 0 && i < M_fil_cnt) ? M_fils[i].label : ""; }
bool printer_filsensor_detected(int i) { return (i >= 0 && i < M_fil_cnt) && M_fils[i].detected; }
bool printer_filsensor_enabled(int i)  { return (i >= 0 && i < M_fil_cnt) && M_fils[i].enabled; }
void printer_filsensor_set_enabled(int i, bool en)
{
    if (!klipper_active() || i < 0 || i >= M_fil_cnt) return;
    char g[96];
    snprintf(g, sizeof(g), "SET_FILAMENT_SENSOR SENSOR=%s ENABLE=%d",
             M_fils[i].label, en ? 1 : 0);
    klipper_gcode_script(g);
}

/* 限位查询用文档化的结构化 RPC（fluidd 同款端点），不走 gcode 文本回流：
 * 无控制台噪音、无解析歧义。M119/QUERY_ENDSTOPS 的文本行仍解析兜底。 */
static void on_endstops_result(char *result_json, void *ud)
{
    LV_UNUSED(ud);
    cJSON *root = cJSON_Parse(result_json ? result_json : "");
    free(result_json);
    if (root) {
        static const char *axes[] = { "x", "y", "z" };
        for (int i = 0; i < 3; i++) {
            cJSON *v = cJSON_GetObjectItem(root, axes[i]);
            if (cJSON_IsString(v) && v->valuestring)
                M_endstop[i] = strcmp(v->valuestring, "TRIGGERED") == 0 ? 1 : 0;
        }
        M_endstop_ms = lv_tick_get();
        M_endstop_fresh = true;
    }
    cJSON_Delete(root);
    refresh();
}

void printer_endstop_refresh(void)
{
    if (!klipper_active()) return;
    moonraker_rpc("printer.query_endstops.status", NULL, on_endstops_result, NULL);
}

int printer_endstop_state(int axis)
{
    if (axis < 0 || axis > 2 || !M_endstop_fresh) return -1;
    return M_endstop[axis];
}

uint32_t printer_endstop_age_ms(void)
{
    return M_endstop_fresh ? lv_tick_elaps(M_endstop_ms) : UINT32_MAX;
}

/* ---- 控制台 ---- */
static void console_append(const char *text, int kind)
{
    if (!text || !text[0]) return;
    int idx;
    if (M_con_cnt < CONSOLE_MAX_LINES) {
        idx = (M_con_head + M_con_cnt) % CONSOLE_MAX_LINES;
        M_con_cnt++;
    } else {
        idx = M_con_head;                     /* 覆盖最旧 */
        M_con_head = (M_con_head + 1) % CONSOLE_MAX_LINES;
    }
    copy_text(M_con[idx].text, sizeof(M_con[idx].text), text);
    M_con[idx].kind = (uint8_t)kind;
    refresh();
}

void printer_console_send(const char *cmd)
{
    if (!klipper_active() || !cmd || !cmd[0]) return;
    char line[CONSOLE_LINE_MAX + 4];
    snprintf(line, sizeof(line), "> %s", cmd);
    console_append(line, 1);
    klipper_gcode_script(cmd);
}

int printer_console_line_count(void) { return M_con_cnt; }

const char *printer_console_line(int i, int *kind)
{
    if (i < 0 || i >= M_con_cnt) { if (kind) *kind = 0; return ""; }
    int idx = (M_con_head + i) % CONSOLE_MAX_LINES;
    if (kind) *kind = M_con[idx].kind;
    return M_con[idx].text;
}

void printer_console_clear(void)
{
    M_con_head = M_con_cnt = 0;
    refresh();
}

/* 温度轮询行（ok B:.. T:..）不进控制台 */
static bool console_is_temp_line(const char *s)
{
    if (strncmp(s, "ok ", 3) == 0) s += 3;
    if (s[0] == 'B' || s[0] == 'C') return s[1] == ':';
    if (s[0] == 'T') {
        const char *p = s + 1;
        while (*p >= '0' && *p <= '9') p++;
        return *p == ':';
    }
    return false;
}

void printer_console_load_history(void)
{
    /* 桌面客户端（winhttp/posix）与 ESP32 客户端都实现 moonraker_rpc */
    moonraker_rpc("server.gcode_store", "{\"count\":100}", on_gcode_store, NULL);
}

static void on_gcode_store(char *result_json, void *ud)
{
    LV_UNUSED(ud);
    cJSON *root = cJSON_Parse(result_json ? result_json : "");
    free(result_json);
    cJSON *store = root ? cJSON_GetObjectItem(root, "gcode_store") : NULL;
    if (cJSON_IsArray(store)) {
        /* 服务端历史是"权威快照"：整环替换，否则每次进页面都叠加一份（重复 5+5） */
        M_con_head = M_con_cnt = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, store) {
            cJSON *type = cJSON_GetObjectItem(it, "type");
            cJSON *msg = cJSON_GetObjectItem(it, "message");
            if (!cJSON_IsString(msg) || !msg->valuestring) continue;
            const char *m = msg->valuestring;
            if (console_is_temp_line(m)) continue;
            int kind = 0;
            if (cJSON_IsString(type) && strcmp(type->valuestring, "command") == 0)
                kind = 1;
            else if (strncmp(m, "!!", 2) == 0) kind = 2;
            else if (strncmp(m, "//", 2) == 0) kind = 3;
            if (kind == 1) {
                char line[CONSOLE_LINE_MAX + 4];
                snprintf(line, sizeof(line), "> %s", m);
                console_append(line, 1);
            } else {
                console_append(m + (kind == 2 ? 2 : kind == 3 ? 2 : 0), kind);
            }
        }
    }
    cJSON_Delete(root);
    refresh();
}

/* ---- Z 校准 ---- */
static void on_gcode_help(char *result_json, void *ud)
{
    LV_UNUSED(ud);
    cJSON *root = cJSON_Parse(result_json ? result_json : "");
    free(result_json);
    M_zcal_loading = false;
    if (root) {
        M_zcal_cnt = 0;
        if (cJSON_GetObjectItem(root, "PROBE_CALIBRATE"))
            copy_text(M_zcal_cmds[M_zcal_cnt++], 32, "PROBE_CALIBRATE");
        if (M_zcal_cnt < 2 && cJSON_GetObjectItem(root, "Z_ENDSTOP_CALIBRATE"))
            copy_text(M_zcal_cmds[M_zcal_cnt++], 32, "Z_ENDSTOP_CALIBRATE");
        M_zcal_loaded = true;
    }
    cJSON_Delete(root);
    refresh();
}

int printer_zcal_command_count(void)
{
    if (!klipper_active()) return 0;
    if (!M_zcal_loaded && !M_zcal_loading &&
        moonraker_rpc("printer.gcode.help", NULL, on_gcode_help, NULL))
        M_zcal_loading = true;
    return M_zcal_cnt;
}

bool printer_zcal_commands_pending(void)
{
    return klipper_active() && M.online && !M_zcal_loaded;
}

bool printer_probe_present(void)
{
    return klipper_active() && M_probe_present;
}

bool printer_probe_z_offset(float *out)
{
    if (!klipper_active() || !M_probe_z_valid || !out) return false;
    *out = M_probe_z_offset;
    return true;
}

bool printer_homing_origin_z(float *out)
{
    if (!klipper_active() || !M_home_origin_valid || !out) return false;
    *out = M_home_origin_z;
    return true;
}

const char *printer_zcal_command(int i)
{
    return (i >= 0 && i < M_zcal_cnt) ? M_zcal_cmds[i] : "";
}

bool printer_zcal_active(void) { return klipper_active() && M_manual_probe_active; }

void printer_zcal_start(const char *command)
{
    if (!klipper_active() || !command || !command[0]) return;
    char g[160];
    int n = snprintf(g, sizeof(g), "SET_GCODE_OFFSET Z=0\n");
    if (!(M.homed[0] && M.homed[1] && M.homed[2]))
        n += snprintf(g + n, sizeof(g) - n, "G28\n");
    snprintf(g + n, sizeof(g) - n, "%s", command);
    klipper_gcode_script(g);
}

/* 校准中=TESTZ 微调；空闲时=普通相对移动（KlipperScreen zcalibrate 的
 * 抬/降喷嘴按钮双模式，TESTZ 仅在手动探测模式下合法） */
void printer_zcal_testz(float mm)
{
    if (!klipper_active()) return;
    char g[64];
    if (M_manual_probe_active)
        snprintf(g, sizeof(g), "TESTZ Z=%.3f", (double)mm);
    else
        snprintf(g, sizeof(g), "G91\nG1 Z%.3f F600\nG90", (double)mm);
    klipper_gcode_script(g);
}

void printer_zcal_accept(void) { if (klipper_active()) klipper_gcode_script("ACCEPT"); }
void printer_zcal_abort(void)  { if (klipper_active()) klipper_gcode_script("ABORT"); }

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
    if (!online) {
        M.rtt_ms = 0;   /* 断线后延迟值失效 */
        /* 断线时底层 clear_pending 直接丢在途请求且无回调，这里补失败收尾 */
        if (files_req.in_flight) files_req_fail();
    }
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
    /* 文件列表在途请求的超时兜底：应答被底层整条丢弃时（大列表 OOM 等）
     * 主动判失败，防面板卡死在"加载中"。状态推送在线时约 4Hz，顺带巡检。 */
    if (files_req.in_flight && lv_tick_elaps(files_req.start_ms) > FILES_REQ_TIMEOUT_MS)
        files_req_fail();

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
    /* 菜单页扩展对象的增量合入（订阅集在 client 按 objects.list 动态生成） */
    for (int i = 0; i < M_fan_cnt; i++) {
        cJSON *f = cJSON_GetObjectItem(status, M_fans[i].name);
        if (!f) continue;
        cJSON *s = cJSON_GetObjectItem(f, "speed");
        if (cJSON_IsNumber(s)) M_fans[i].speed = (float)s->valuedouble;
    }
    for (int i = 0; i < M_fil_cnt; i++) {
        cJSON *f = cJSON_GetObjectItem(status, M_fils[i].name);
        if (!f) continue;
        cJSON *en = cJSON_GetObjectItem(f, "enabled");
        if (cJSON_IsBool(en)) M_fils[i].enabled = cJSON_IsTrue(en);
        cJSON *det = cJSON_GetObjectItem(f, "filament_detected");
        if (cJSON_IsBool(det)) M_fils[i].detected = cJSON_IsTrue(det);
    }
    if ((it = cJSON_GetObjectItem(status, "manual_probe"))) {
        cJSON *a = cJSON_GetObjectItem(it, "is_active");
        if (cJSON_IsBool(a)) M_manual_probe_active = cJSON_IsTrue(a);
    }
    if ((it = cJSON_GetObjectItem(status, "probe")) ||
        (it = cJSON_GetObjectItem(status, "bltouch"))) {
        cJSON *z = cJSON_GetObjectItem(it, "z_offset");
        if (cJSON_IsNumber(z)) { M_probe_z_offset = (float)z->valuedouble; M_probe_z_valid = true; }
    }
    if ((it = cJSON_GetObjectItem(status, "gcode_move"))) {
        cJSON *ho = cJSON_GetObjectItem(it, "homing_origin");
        if (cJSON_IsArray(ho)) {
            cJSON *z = cJSON_GetArrayItem(ho, 2);
            if (cJSON_IsNumber(z)) { M_home_origin_z = (float)z->valuedouble; M_home_origin_valid = true; }
        }
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
