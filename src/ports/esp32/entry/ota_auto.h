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
