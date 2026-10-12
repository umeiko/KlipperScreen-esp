#pragma once

/* 开机自动 OTA 检测：连上 WiFi 后检查固件服务器，有新版自动升级重启 */
void ota_auto_start(void);

/* OTA 升级进度（供屏幕 UI 面板轮询显示） */
typedef struct {
    volatile bool  active;       /* 1=升级进行中（检测到新版→下载→写入→重启） */
    volatile int   phase;        /* 0=检测到新版 1=下载中 2=写入完成 3=完成/即将重启 */
    volatile int   total;        /* 固件总字节（Content-Length；0=未知） */
    volatile int   downloaded;   /* 已下载字节 */
    char cur_ver[32];            /* 当前版本 */
    char new_ver[32];            /* 目标版本 */
} ota_progress_t;

/* 返回全局进度状态指针（UI 线程轮询；无需加锁，字段为 volatile 简单类型） */
const ota_progress_t *ota_progress_get(void);

/* ---- 手动 OTA（设置菜单入口） ---- */

/* 手动检查状态（UI 轮询显示） */
typedef struct {
    volatile bool  checking;   /* 1=检查请求进行中 */
    volatile int   result;     /* 0=已是最新 1=有新版本 -1=失败（未连WiFi/未配置/网络错） */
    char remote[32];           /* 服务器版本（result==1 时有效） */
} ota_check_state_t;

const ota_check_state_t *ota_check_state_get(void);

/* 异步检查服务器新版本；成功后结果写入 ota_check_state_get() */
void ota_check_start(void);

/* 用最近检查到的新版本开始升级（异步下载→写入→重启）；返回 0=已启动，-1=不可用 */
int ota_start_update(void);
