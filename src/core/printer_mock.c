/*
 * Mock 打印机数据层（desktop/截图演示用；esp32 使用 printer_model.c）。
 * 由 mock_printer.c 改名而来，逻辑不变。
 */
#include "printer.h"
#include "app_settings.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

/* UI 注入的刷新回调（panel_mgr_tick） */
static void (*refresh_hook)(void);
void printer_set_refresh_hook(void (*fn)(void)) { refresh_hook = fn; }

static struct {
    printer_state_t state;
    float bed, bed_t;
    float pos[3];
    int homed[3];
    float e_pos;
    int progress;          /* 千分比 */
    const char *filename;
    uint32_t tick;         /* 秒计数 */
    uint32_t print_start_tick;
    float flow;
    int tool_count;        /* 工具数，>=1 */
    int current_tool;      /* 活动工具，0 起 */
    float tool_temp[PRINTER_MAX_TOOLS];
    float tool_target[PRINTER_MAX_TOOLS];
} P = {
    .state = PRINTER_STATE_STANDBY,
    .bed = 23.8f, .bed_t = 0,
    .pos = {0, 0, 0}, .homed = {0, 0, 0},
    .progress = 0, .filename = "", .flow = 100,
    .tool_count = 1, .current_tool = 0,
    .tool_temp = {24.5f}, .tool_target = {0},
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

/* 演示多工具：注入后可显示 T0..T{n-1} 选择器 */
void printer_mock_set_tool_count(int n)
{
    if (n < 1) n = 1;
    if (n > PRINTER_MAX_TOOLS) n = PRINTER_MAX_TOOLS;
    P.tool_count = n;
    if (P.current_tool >= n) P.current_tool = 0;
}

int printer_tool_count(void)  { return P.tool_count >= 1 ? P.tool_count : 1; }

int printer_current_tool(void)
{
    int t = P.current_tool;
    if (t < 0 || t >= printer_tool_count()) return 0;
    return t;
}

float printer_temp_tool(int tool)
{
    if (tool < 0 || tool >= printer_tool_count()) tool = 0;
    return P.tool_temp[tool];
}

float printer_target_tool(int tool)
{
    if (tool < 0 || tool >= printer_tool_count()) tool = 0;
    return P.tool_target[tool];
}

void printer_set_target_tool(int tool, float t)
{
    if (tool < 0 || tool >= P.tool_count) return;
    P.tool_target[tool] = t;
}

void printer_select_tool(int tool)
{
    if (tool < 0 || tool >= P.tool_count) return;
    P.current_tool = tool;
}

float printer_temp_ext(void)  { return printer_temp_tool(printer_current_tool()); }
float printer_temp_bed(void)  { return P.bed; }
float printer_target_ext(void){ return printer_target_tool(printer_current_tool()); }
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

void printer_set_target_ext(float t) { P.tool_target[printer_current_tool()] = t; }
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
    for (int i = 0; i < PRINTER_MAX_TOOLS; i++) P.tool_target[i] = 0;
    P.bed_t = 0;
}

void printer_firmware_restart(void)
{
    /* FIRMWARE_RESTART：下位机重启期间加热目标清零 */
    for (int i = 0; i < PRINTER_MAX_TOOLS; i++) P.tool_target[i] = 0;
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

    for (int i = 0; i < PRINTER_MAX_TOOLS; i++)
        P.tool_temp[i] += (P.tool_target[i] - P.tool_temp[i]) * 0.18f +
                          ((float)(rand() % 10) - 5) * 0.02f;
    P.bed += (P.bed_t - P.bed) * 0.12f + ((float)(rand() % 10) - 5) * 0.015f;

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
