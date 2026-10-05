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
void printer_home(int axis);                  /* 0/1/2=X/Y/Z，3=XY，-1=全部归位 */
void printer_motors_off(void);                /* M84：关闭全部步进电机 */
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

#define PRINTER_FILES_PAGE_SIZE 8
typedef struct {
    printer_file_t files[PRINTER_FILES_PAGE_SIZE];
    int count;               /* -1 = failed; 0 = empty */
    unsigned total;          /* valid files, not just visible rows */
    unsigned skipped;        /* paths too long for the existing print API */
} printer_file_page_t;

/* Bounded page, borrowed only during cb (LVGL context). No heap ownership transfer.
 * offset is a valid-file index. Cancel invalidates late replies when leaving.
 * Poll from the UI tick, independently of printer status notifications. */
typedef void (*printer_files_cb)(const printer_file_page_t *page, void *ud);
bool printer_files_refresh(unsigned offset, printer_files_cb cb, void *ud);
void printer_files_poll(void);
void printer_files_cancel(void);

void printer_file_delete(const char *name);   /* 删除 gcodes 下的文件 */

/* ---- 菜单页扩展能力（Klipper only；ESP32 客户端暂未接线，返回空/默认） ---- */

/* 宏：来自 objects.list 的 "gcode_macro *"（剔除 '_' 开头与 LOAD/UNLOAD_FILAMENT）。
 * label 为显示名（下划线转空格）；run 发送宏名本体（无参数）。 */
int  printer_macro_count(void);
const char *printer_macro_name(int i);
const char *printer_macro_label(int i);
void printer_macro_run(int i);

/* 风扇：fan / fan_generic 可写（M106 / SET_FAN_SPEED），heater_fan / controller_fan 只读。
 * speed 为 0..1（未订阅到为 -1）。 */
int  printer_fan_count(void);
const char *printer_fan_name(int i);
float printer_fan_speed(int i);
bool printer_fan_writable(int i);
void printer_fan_set(int i, float speed);

/* 断料传感器（filament_switch_sensor / filament_motion_sensor） */
int  printer_filsensor_count(void);
const char *printer_filsensor_name(int i);
bool printer_filsensor_detected(int i);   /* 有料 */
bool printer_filsensor_enabled(int i);
void printer_filsensor_set_enabled(int i, bool en);

/* XYZ 限位：QUERY_ENDSTOPS 的结果经 gcode 响应文本（"x:open"）回流解析。
 * refresh 发送查询；state：-1 未知 / 0 未触发 / 1 触发。 */
void printer_endstop_refresh(void);
int  printer_endstop_state(int axis);     /* 0=X 1=Y 2=Z */
uint32_t printer_endstop_age_ms(void);    /* UINT32_MAX = 从未刷新 */

/* 控制台：发送 + 历史环形缓冲（新行追加；notify_gcode_response 全局推送）。
 * line(0) 最旧。kind：0 普通响应，1 本机发出的命令，2 错误(!!)，3 警告(//)。 */
void printer_console_send(const char *cmd);
int  printer_console_line_count(void);
const char *printer_console_line(int i, int *kind);
void printer_console_clear(void);
void printer_console_load_history(void);  /* 拉取 server.gcode_store（打开控制台时） */

/* Z 校准：PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE 的可用性来自 printer.gcode.help；
 * 校准活跃态来自 manual_probe.is_active 订阅。 */
int  printer_zcal_command_count(void);
const char *printer_zcal_command(int i);   /* "PROBE_CALIBRATE" 等 */
bool printer_zcal_commands_pending(void);  /* gcode.help 还在路上 */
bool printer_probe_present(void);          /* objects.list 有 probe/bltouch 等 */
bool printer_probe_z_offset(float *out);   /* 已保存的探测偏移（订阅 probe/bltouch） */
bool printer_homing_origin_z(float *out);  /* 校准中的新偏移（gcode_move.homing_origin[2]） */
bool printer_zcal_active(void);
void printer_zcal_start(const char *command);
void printer_zcal_testz(float mm);
void printer_zcal_accept(void);
void printer_zcal_abort(void);

#ifdef __cplusplus
}
#endif
