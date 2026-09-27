#pragma once

/*
 * WiFi 抽象：扫描 → 选 AP → 输密码 → 连接，三端实现
 *   esp32   : bsp/esp32/bsp_wifi_esp32.c     （esp_wifi 事件驱动）
 *   Windows : bsp/desktop/bsp_wifi_windows.c （netsh wlan，后台线程 + 隐藏子进程）
 *   Linux   : bsp/desktop/bsp_wifi_linux.c   （nmcli，后台线程 + popen）
 *
 * 全部为非阻塞轮询模型，UI 在节拍里 poll，不做任何回调注册。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_WIFI_SSID_MAX 32
#define BSP_WIFI_PASS_MAX 63

typedef struct {
    char ssid[BSP_WIFI_SSID_MAX + 1];
    int  rssi;      /* dBm，桌面端可能只有信号百分比换算值 */
    bool secure;    /* 需要密码 */
} bsp_wifi_ap_t;

typedef enum {
    BSP_WIFI_IDLE = 0,    /* 未在连接 */
    BSP_WIFI_CONNECTING,  /* 连接中 */
    BSP_WIFI_CONNECTED,   /* 已连接（拿到 IP / 接口 associated） */
    BSP_WIFI_FAILED,      /* 失败（密码错误/超时/不可达） */
} bsp_wifi_state_t;

/* 扫描中/扫描失败的 poll 返回值 */
#define BSP_WIFI_SCAN_RUNNING (-1)
#define BSP_WIFI_SCAN_FAILED  (-2)

void bsp_wifi_init(void);

/* 启动一次扫描（进行中重复调用忽略） */
void bsp_wifi_scan_start(void);

/* 轮询扫描结果：>=0 为结果条数（完成），BSP_WIFI_SCAN_RUNNING/FAILED 为未完成 */
int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max);

/* 发起连接；open 网络传 password=NULL。自动取消正在进行的连接 */
void bsp_wifi_connect(const char *ssid, const char *password);

bsp_wifi_state_t bsp_wifi_status(void);

/* 当前是否真的连着 WiFi（不依赖本 UI 的连接流程；桌面端为后台轮询缓存） */
bool bsp_wifi_connected(void);

#ifdef __cplusplus
}
#endif
