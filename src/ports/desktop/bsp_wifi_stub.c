/*
 * WiFi 空实现：macOS 桌面端。
 * Windows(netsh wlan) / Linux(nmcli) 的真实后端都带 OS 守卫，macOS 上两个都编不进去，
 * 这里补符号让链接通过：WiFi 页图标常灰，扫描直接报失败，连接请求立即失败。
 * 仅在 APPLE 构建时被 CMake 收进目标。
 */
#include "bsp_wifi.h"

#ifdef __APPLE__

void bsp_wifi_init(void) {}

void bsp_wifi_scan_start(void) {}

int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max)
{
    (void)out; (void)max;
    return BSP_WIFI_SCAN_FAILED;
}

void bsp_wifi_connect(const char *ssid, const char *password)
{
    (void)ssid; (void)password;
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    return BSP_WIFI_IDLE;
}

bool bsp_wifi_connected(void)
{
    return false;
}

#endif /* __APPLE__ */
