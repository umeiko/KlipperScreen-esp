/*
 * Mock 打印机数据层（desktop/截图演示用；esp32 使用 printer_model.c）。
 * 由 mock_printer.c 改名而来，逻辑不变。
 */
#include "printer.h"
#include "app_settings.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UI 注入的刷新回调（panel_mgr_tick） */
static void (*refresh_hook)(void);
void printer_set_refresh_hook(void (*fn)(void)) { refresh_hook = fn; }

static struct {
    printer_state_t state;
    float ext, bed, ext_t, bed_t;
    float pos[3];
    int homed[3];
    float e_pos;
    int progress;          /* 千分比 */
    const char *filename;
    uint32_t tick;         /* 秒计数 */
    uint32_t print_start_tick;
    float flow;
} P = {
    .state = PRINTER_STATE_STANDBY,
    .ext = 24.5f, .bed = 23.8f, .ext_t = 0, .bed_t = 0,
    .pos = {0, 0, 0}, .homed = {0, 0, 0},
    .progress = 0, .filename = "", .flow = 100,
};

printer_state_t printer_state(void) { return P.state; }

printer_capabilities_t printer_capabilities(void)
{
    if (settings_load_machine_mode() == MACHINE_MODE_KLIPPER)
        return PRINTER_CAP_KLIPPER_ALL;
    if (settings_load_bambu_link() == BAMBU_LINK_CLOUD_MONITOR)
        return 0;   /* 云端接口只监视状态 */
    return PRINTER_CAP_TEMP_CONTROL | PRINTER_CAP_MOVE | PRINTER_CAP_EXTRUDE |
           PRINTER_CAP_FILES | PRINTER_CAP_PRINT_START | PRINTER_CAP_PAUSE |
           PRINTER_CAP_RESUME | PRINTER_CAP_CANCEL;
}

/* 截图演示用：直接注入状态（desktop 端 main.c 调用） */
void printer_mock_set_state(printer_state_t s) { P.state = s; }
float printer_temp_ext(void)  { return P.ext; }
float printer_temp_bed(void)  { return P.bed; }
float printer_target_ext(void){ return P.ext_t; }
float printer_target_bed(void){ return P.bed_t; }
float printer_pos(int axis)   { return P.pos[axis]; }
int printer_homed(int axis)   { return P.homed[axis]; }
int printer_progress_permille(void) { return P.progress; }
const char *printer_filename(void)  { return P.filename; }
float printer_flow_pct(void)  { return P.flow; }
int  printer_rtt_ms(void)     { return 0; }   /* mock 无真实连接 */

bool printer_take_error(char *out, size_t cap)   /* mock 不产生 klippy 错误 */
{
    (void)out; (void)cap;
    return false;
}

uint32_t printer_print_elapsed_s(void)
{
    return P.state == PRINTER_STATE_PRINTING || P.state == PRINTER_STATE_PAUSED
           ? P.tick - P.print_start_tick : 0;
}

uint32_t printer_print_eta_s(void)
{
    if (P.state != PRINTER_STATE_PRINTING || P.progress < 5) return 0;
    uint32_t el = printer_print_elapsed_s();
    return el * (1000 - P.progress) / P.progress;
}

int printer_layer_current(void) { return 0; }
int printer_layer_total(void) { return 0; }

void printer_set_target_ext(float t) { P.ext_t = t; }
void printer_set_target_bed(float t) { P.bed_t = t; }

void printer_jog(int axis, float dist)
{
    if (axis < 0 || axis > 2) return;
    P.pos[axis] += dist;
    if (P.pos[axis] < 0) P.pos[axis] = 0;
    if (P.pos[axis] > 235) P.pos[axis] = 235;
}

void printer_home(int axis)
{
    if (axis < 0) { P.homed[0] = P.homed[1] = P.homed[2] = 1; P.pos[0] = P.pos[1] = P.pos[2] = 0; }
    else if (axis == 3) { P.homed[0] = P.homed[1] = 1; P.pos[0] = P.pos[1] = 0; }
    else { P.homed[axis] = 1; P.pos[axis] = 0; }
}

void printer_extrude(float mm) { P.e_pos += mm; }

void printer_motors_off(void) { /* M84：mock 无保持力矩概念，状态不变 */ }

void printer_print_start(const char *filename)
{
    P.filename = filename;
    P.progress = 0;
    P.print_start_tick = P.tick;
    P.state = PRINTER_STATE_PRINTING;
}

void printer_print_pause(void)  { if (P.state == PRINTER_STATE_PRINTING) P.state = PRINTER_STATE_PAUSED; }
void printer_print_resume(void) { if (P.state == PRINTER_STATE_PAUSED)  P.state = PRINTER_STATE_PRINTING; }
void printer_print_cancel(void) { P.state = PRINTER_STATE_STANDBY; P.progress = 0; P.filename = ""; }

void printer_emergency_stop(void)
{
    /* M112：立刻停一切运动与加热 */
    P.state = PRINTER_STATE_STANDBY;
    P.progress = 0;
    P.filename = "";
    P.ext_t = 0;
    P.bed_t = 0;
}

void printer_firmware_restart(void)
{
    /* FIRMWARE_RESTART：下位机重启期间加热目标清零 */
    P.ext_t = 0;
    P.bed_t = 0;
}

/* ---------- GCode 文件列表（本地模拟，删除后从列表消失） ---------- */
static struct { const char *name; uint32_t size; double modified; bool deleted; } mock_files[] = {
    {"calibration_cube.gcode",  420 * 1024,        1756600000, false},
    {"3dbenchy.gcode",          3360 * 1024,       1756700000, false},
    {"voron_cube.gcode",        13400 * 1024,      1756800000, false},
    {"fan_duct_v2.gcode",       5350 * 1024,       1756900000, false},
    {"phone_stand.gcode",       9030 * 1024,       1757000000, false},
    {"ercf_gate.gcode",         1990 * 1024,       1757100000, false},
};

bool printer_files_refresh(unsigned offset, printer_files_cb cb, void *ud)
{
    if (!cb) return false;
    int n = 0;
    for (unsigned i = 0; i < sizeof(mock_files) / sizeof(mock_files[0]); i++)
        if (!mock_files[i].deleted) n++;
    printer_file_page_t page = { .total = n };
    int k = 0;
    for (unsigned i = 0; i < sizeof(mock_files) / sizeof(mock_files[0]); i++) {
        if (mock_files[i].deleted) continue;
        if (k++ < (int)offset || page.count >= PRINTER_FILES_PAGE_SIZE) continue;
        printer_file_t *f = &page.files[page.count++];
        strncpy(f->name, mock_files[i].name, sizeof(f->name) - 1);
        f->size = mock_files[i].size;
        f->modified = mock_files[i].modified;
    }
    cb(&page, ud);   /* mock 同步回调（已在 LVGL 上下文） */
    return true;
}

void printer_files_cancel(void) {}
void printer_files_poll(void) {}

void printer_file_delete(const char *name)
{
    for (unsigned i = 0; i < sizeof(mock_files) / sizeof(mock_files[0]); i++)
        if (!mock_files[i].deleted && strcmp(mock_files[i].name, name) == 0) {
            mock_files[i].deleted = true;
            return;
        }
}

/* 每秒：温度向目标漂移 + 打印进度推进 + 广播节拍 */
static void tick_1s(lv_timer_t *tm)
{
    LV_UNUSED(tm);
    P.tick++;

    float rate = (P.ext_t > P.ext) ? 3.0f : 0.4f;   /* 升温快、降温慢 */
    P.ext += (P.ext_t - P.ext) * 0.18f + ((float)(rand() % 10) - 5) * 0.02f;
    P.bed += (P.bed_t - P.bed) * 0.12f + ((float)(rand() % 10) - 5) * 0.015f;
    LV_UNUSED(rate);

    if (P.state == PRINTER_STATE_PRINTING && P.progress < 1000) {
        P.progress += 2 + rand() % 3;               /* ~5~8 分钟打完一个 mock 件 */
        if (P.progress >= 1000) { P.progress = 1000; P.state = PRINTER_STATE_COMPLETE; }
    }

    if (refresh_hook) refresh_hook();
}

void printer_init(void)
{
    lv_timer_create(tick_1s, 1000, NULL);
}

/* ---------- 菜单页扩展能力的模拟数据（宏/风扇/断料/限位/控制台/Z 校准） ---------- */
static void mock_con_add(const char *text, int kind);

static struct { const char *name; const char *label; } mock_macros[] = {
    {"PRINT_START",   "PRINT_START"},
    {"END_PRINT",     "END_PRINT"},
    {"CLEAN_NOZZLE",  "CLEAN_NOZZLE"},
    {"PARK_TOOLHEAD", "PARK_TOOLHEAD"},
    {"M900",          "M900"},
    {"SET_PRESSURE_ADVANCE", "SET_PRESSURE_ADVANCE"},
};
int  printer_macro_count(void) { return (int)(sizeof(mock_macros) / sizeof(mock_macros[0])); }
const char *printer_macro_name(int i)  { return (i >= 0 && i < printer_macro_count()) ? mock_macros[i].name : ""; }
const char *printer_macro_label(int i) { return (i >= 0 && i < printer_macro_count()) ? mock_macros[i].label : ""; }
void printer_macro_run(int i)
{
    if (i < 0 || i >= printer_macro_count()) return;
    char line[64];
    snprintf(line, sizeof(line), "> %s", mock_macros[i].name);
    mock_con_add(line, 1);   /* 进控制台记录 */
    mock_con_add("// 宏已执行（模拟）", 3);
}

/* 宏参数（模拟）：M900 → K|default(0.5)|float；SET_PRESSURE_ADVANCE → ADVANCE|default(0.05)|float */
static struct { const char *name; const char *dflt; bool numeric; } mock_params_m900[] = {
    {"K", "0.5", true},
};
static struct { const char *name; const char *dflt; bool numeric; } mock_params_pa[] = {
    {"ADVANCE", "0.05", true},
};

static int mock_param_list(int macro_idx, const void **out)
{
    if (macro_idx == 4) { *out = mock_params_m900; return 1; }
    if (macro_idx == 5) { *out = mock_params_pa; return 1; }
    *out = NULL;
    return 0;
}

bool printer_macro_params_loading(void) { return false; }
int  printer_macro_param_count(int macro_idx)
{
    const void *list;
    return mock_param_list(macro_idx, &list);
}

bool printer_macro_param_info(int macro_idx, int p, char *name, size_t name_cap,
                              char *dflt, size_t dflt_cap, bool *is_numeric)
{
    const void *list;
    int cnt = mock_param_list(macro_idx, &list);
    if (p < 0 || p >= cnt) return false;
    const typeof(mock_params_m900[0]) *pr = list;
    strncpy(name, pr[p].name, name_cap - 1);
    name[name_cap - 1] = 0;
    strncpy(dflt, pr[p].dflt, dflt_cap - 1);
    dflt[dflt_cap - 1] = 0;
    if (is_numeric) *is_numeric = pr[p].numeric;
    return true;
}

void printer_macro_run_with(int macro_idx, const char *const *values)
{
    if (macro_idx < 0 || macro_idx >= printer_macro_count()) return;
    const char *name = mock_macros[macro_idx].name;
    int pcnt = printer_macro_param_count(macro_idx);
    char line[128];
    int n = snprintf(line, sizeof(line), "> %s", name);
    bool gcmd = (name[0] == 'G' || name[0] == 'M');
    for (int p = 0; p < pcnt && n > 0 && n < (int)sizeof(line) - 1; p++) {
        if (!values || !values[p] || !values[p][0]) continue;
        char pname[32], d[24];
        if (!printer_macro_param_info(macro_idx, p, pname, sizeof(pname), d, sizeof(d), NULL))
            continue;
        n += snprintf(line + n, sizeof(line) - n, gcmd ? " %s%s" : " %s=%s", pname, values[p]);
    }
    mock_con_add(line, 1);
    mock_con_add("// 宏已执行（模拟）", 3);
}

static struct { const char *label; bool writable; float speed; } mock_fans[] = {
    {"fan",     true,  0.65f},
    {"chamber", true,  0.30f},
    {"hotend",  false, 1.00f},
};
int  printer_fan_count(void) { return (int)(sizeof(mock_fans) / sizeof(mock_fans[0])); }
const char *printer_fan_name(int i) { return (i >= 0 && i < printer_fan_count()) ? mock_fans[i].label : ""; }
float printer_fan_speed(int i)      { return (i >= 0 && i < printer_fan_count()) ? mock_fans[i].speed : -1; }
bool printer_fan_writable(int i)    { return (i >= 0 && i < printer_fan_count()) && mock_fans[i].writable; }
void printer_fan_set(int i, float speed)
{
    if (i < 0 || i >= printer_fan_count() || !mock_fans[i].writable) return;
    mock_fans[i].speed = speed < 0 ? 0 : speed > 1 ? 1 : speed;
}

static bool mock_fil_enabled = true, mock_fil_detected = true;
int  printer_filsensor_count(void) { return 1; }
const char *printer_filsensor_name(int i) { return i == 0 ? "runout" : ""; }
bool printer_filsensor_detected(int i) { (void)i; return mock_fil_detected; }
bool printer_filsensor_enabled(int i)  { (void)i; return mock_fil_enabled; }
void printer_filsensor_set_enabled(int i, bool en) { (void)i; mock_fil_enabled = en; }

static int mock_endstop[3] = { 0, 0, 1 };   /* z 触发，x/y 未触发 */
static uint32_t mock_endstop_ms;
void printer_endstop_refresh(void) { mock_endstop_ms = lv_tick_get(); }
int  printer_endstop_state(int axis) { return (axis >= 0 && axis < 3 && mock_endstop_ms) ? mock_endstop[axis] : -1; }
uint32_t printer_endstop_age_ms(void)
{
    return mock_endstop_ms ? lv_tick_elaps(mock_endstop_ms) : UINT32_MAX;
}

#define MOCK_CON_MAX 80
static struct { char text[96]; uint8_t kind; } mock_con[MOCK_CON_MAX];
static int mock_con_head, mock_con_cnt;

static void mock_con_add(const char *text, int kind)
{
    int idx;
    if (mock_con_cnt < MOCK_CON_MAX) {
        idx = (mock_con_head + mock_con_cnt) % MOCK_CON_MAX;
        mock_con_cnt++;
    } else {
        idx = mock_con_head;
        mock_con_head = (mock_con_head + 1) % MOCK_CON_MAX;
    }
    strncpy(mock_con[idx].text, text, sizeof(mock_con[idx].text) - 1);
    mock_con[idx].text[sizeof(mock_con[idx].text) - 1] = 0;
    mock_con[idx].kind = (uint8_t)kind;
}

void printer_console_send(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    char line[100];
    snprintf(line, sizeof(line), "> %s", cmd);
    mock_con_add(line, 1);
    if (strcmp(cmd, "QUERY_ENDSTOPS") == 0) {
        /* 与真实端一致：限位回流只进传感器页，不进控制台 */
        mock_endstop_ms = lv_tick_get();
    } else if (strncmp(cmd, "G28", 3) == 0) {
        P.homed[0] = P.homed[1] = P.homed[2] = 1;
        mock_con_add("ok", 0);
    } else {
        mock_con_add("ok", 0);
    }
}

int  printer_console_line_count(void) { return mock_con_cnt; }
const char *printer_console_line(int i, int *kind)
{
    if (i < 0 || i >= mock_con_cnt) { if (kind) *kind = 0; return ""; }
    int idx = (mock_con_head + i) % MOCK_CON_MAX;
    if (kind) *kind = mock_con[idx].kind;
    return mock_con[idx].text;
}
void printer_console_clear(void) { mock_con_head = mock_con_cnt = 0; }
void printer_console_load_history(void)
{
    if (mock_con_cnt) return;   /* 只补一次 */
    mock_con_add("// 模拟器控制台历史", 3);
    mock_con_add("> M115", 1);
    mock_con_add("FIRMWARE_NAME:Klipper (mock)", 0);
}

static bool mock_zcal_active;
int  printer_zcal_command_count(void) { return 2; }
bool printer_zcal_commands_pending(void) { return false; }
bool printer_probe_present(void) { return true; }
bool printer_probe_z_offset(float *out) { if (!out) return false; *out = 1.84f; return true; }
bool printer_homing_origin_z(float *out)
{
    if (!out) return false;
    *out = mock_zcal_active ? 1.72f : 0.0f;   /* 校准中演示一个进行中的新偏移 */
    return true;
}
const char *printer_zcal_command(int i)
{
    static const char *cmds[] = { "PROBE_CALIBRATE", "Z_ENDSTOP_CALIBRATE" };
    return (i >= 0 && i < 2) ? cmds[i] : "";
}
bool printer_zcal_active(void) { return mock_zcal_active; }
void printer_zcal_start(const char *command)
{
    (void)command;
    P.homed[0] = P.homed[1] = P.homed[2] = 1;
    mock_zcal_active = true;
}
void printer_zcal_testz(float mm)
{
    P.pos[2] += mm;
    if (P.pos[2] < 0) P.pos[2] = 0;
}
void printer_zcal_accept(void) { mock_zcal_active = false; }
void printer_zcal_abort(void)  { mock_zcal_active = false; }
