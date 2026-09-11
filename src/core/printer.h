#pragma once
/*
 * 打印机数据层接口（原 mock_printer.h 改名扩展）。
 * 两个实现：
 *   printer_mock.c  —— desktop simulator/截图演示用，本地模拟
 *   printer_model.c —— ESP32 与 Windows 真实实现，数据来自 Moonraker WebSocket
 * 面板只通过这里的访问器取数，不感知数据来源。
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRINTER_STATE_STANDBY = 0,
    PRINTER_STATE_PRINTING,
    PRINTER_STATE_PAUSED,
    PRINTER_STATE_COMPLETE,
    PRINTER_STATE_DISCONNECTED,   /* Moonraker 离线 / klippy 未就绪（mock 不会进入） */
    PRINTER_STATE_ERROR,          /* klippy shutdown/error */
} printer_state_t;

/* 后端实际可执行的操作。面板按能力开放入口，同一套 UI 因而可同时承载
 * Klipper、拓竹云端只读监视和拓竹局域网控制。 */
typedef uint32_t printer_capabilities_t;
enum {
    PRINTER_CAP_TEMP_CONTROL     = 1u << 0,
    PRINTER_CAP_MOVE             = 1u << 1,
    PRINTER_CAP_EXTRUDE          = 1u << 2,
    PRINTER_CAP_FILES            = 1u << 3,
    PRINTER_CAP_PRINT_START      = 1u << 4,
    PRINTER_CAP_PAUSE            = 1u << 5,
    PRINTER_CAP_RESUME           = 1u << 6,
    PRINTER_CAP_CANCEL           = 1u << 7,
    PRINTER_CAP_EMERGENCY_STOP   = 1u << 8,
    PRINTER_CAP_FIRMWARE_RESTART = 1u << 9,
};

#define PRINTER_CAP_KLIPPER_ALL ((printer_capabilities_t)((1u << 10) - 1u))

/* 初始化并启动 1s 数据节拍（内部创建 LVGL timer，驱动 panel_mgr_tick） */
void printer_init(void);

/* UI 层注入数据刷新回调（panel_mgr_tick），避免 core 反向依赖 ui */
void printer_set_refresh_hook(void (*fn)(void));
printer_capabilities_t printer_capabilities(void);
static inline bool printer_has_capability(printer_capabilities_t cap)
{
    return (printer_capabilities() & cap) == cap;
}

/* ---- 读 ---- */
printer_state_t printer_state(void);
float printer_temp_ext(void);      /* 当前喷嘴温度 */
float printer_temp_bed(void);
float printer_target_ext(void);    /* 目标温度 */
float printer_target_bed(void);
float printer_pos(int axis);       /* 0=X 1=Y 2=Z */
int  printer_homed(int axis);
int  printer_progress_permille(void);   /* 0~1000 千分比 */
const char *printer_filename(void);
uint32_t printer_print_elapsed_s(void);
uint32_t printer_print_eta_s(void);
int  printer_layer_current(void);       /* 0=后端未提供 */
int  printer_layer_total(void);
float printer_flow_pct(void);      /* 打印流量 % */
int  printer_rtt_ms(void);         /* 到 Moonraker 的应用层心跳延迟 ms，0=未知/离线 */

/* 取走一条待提示的 klippy 错误（如 "Endstop not triggered"，来自 GCode "!!" 响应行）。
 * 有则拷入 out 并返回 true（取后清空），无则 false。UI 节拍轮询后弹 toast。 */
bool printer_take_error(char *out, size_t cap);

/* ---- 写（真实实现经 klipper_api 发 RPC，mock 直接改状态） ---- */
void printer_set_target_ext(float t);
void printer_set_target_bed(float t);
void printer_jog(int axis, float dist);       /* 相对点动 */
void printer_home(int axis);                  /* -1 = 全部归位 */
void printer_extrude(float mm);               /* 正=挤出 负=回抽 */
void printer_print_start(const char *filename);
void printer_print_pause(void);
void printer_print_resume(void);
void printer_print_cancel(void);
void printer_emergency_stop(void);      /* 对应 M112：立即停止 */
void printer_firmware_restart(void);    /* 对应 FIRMWARE_RESTART */

/* ---- GCode 文件（历史打印文件面板） ---- */
typedef struct {
    char     name[96];     /* 相对 gcodes 根的路径（可能含子目录） */
    uint32_t size;         /* 字节 */
    double   modified;     /* epoch 秒 */
} printer_file_t;

/* 异步刷新 gcodes 文件列表。回调在 LVGL 上下文执行；
 * files 为堆数组（回调负责 free），count<=0 表示离线/失败/无文件。
 * 返回 false = 请求未发出（离线或上一个请求未完成）。 */
typedef void (*printer_files_cb)(printer_file_t *files, int count, void *ud);
bool printer_files_refresh(printer_files_cb cb, void *ud);

void printer_file_delete(const char *name);   /* 删除 gcodes 下的文件 */

#ifdef __cplusplus
}
#endif
