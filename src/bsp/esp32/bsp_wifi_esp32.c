/*
 * WiFi 实现：ESP32（esp_wifi，事件驱动，STA 模式）
 * nvs/netif/event loop 在这里惰性初始化，bsp 显示部分不依赖网络。
 */
#include "../bsp_wifi.h"
#include "../bsp.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "bsp_wifi"
#define SCAN_MAX 16
#define RECONNECT_DELAY_US (2 * 1000 * 1000)

/* 时间来源完全走内网：Moonraker（Tornado）HTTP 响应自带 Date 头（GMT），
   不依赖外网 NTP。时区固定 CST-8（中国标准时间）。 */
static esp_err_t http_date_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        strcasecmp(evt->header_key, "Date") == 0) {
        struct tm tmv = {0};
        /* "Wed, 04 Sep 2026 01:35:22 GMT" */
        if (strptime(evt->header_value, "%a, %d %b %Y %H:%M:%S", &tmv)) {
            /* newlib 无 timegm：临时切 UTC 求 epoch 再切回 CST-8 */
            setenv("TZ", "UTC0", 1); tzset();
            time_t t = mktime(&tmv);
            setenv("TZ", "CST-8", 1); tzset();
            if (t > 0) {
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "time synced from moonraker: %s", evt->header_value);
            }
        }
    }
    return ESP_OK;
}

static void time_sync_task(void *arg)
{
    char *url = arg;   /* 由调用方 malloc，这里释放 */
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_date_cb,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        if (esp_http_client_perform(c) != ESP_OK)
            ESP_LOGW(TAG, "time sync http failed");
        esp_http_client_cleanup(c);
    }
    free(url);
    vTaskDelete(NULL);
}

void bsp_time_sync_from_host(const char *host, uint16_t port)
{
    char *url = malloc(96);
    if (!url) return;
    snprintf(url, 96, "http://%s:%u/", host, (unsigned)port);
    /* 异步：HTTP 最多阻塞 3s，不能卡在 websocket 事件回调里 */
    if (xTaskCreate(time_sync_task, "tsync", 4096, url, 5, NULL) != pdPASS)
        free(url);
}

static struct {
    bool            inited;
    bool            scan_running;
    bool            scan_done;
    wifi_ap_record_t records[SCAN_MAX];
    uint16_t        record_num;
    bsp_wifi_state_t state;
    bool            connected;      /* GOT_IP / DISCONNECTED 事件维护 */
    bool            manual;         /* 主动断开（切换 AP），不做自动重连 */
    esp_timer_handle_t reconnect_timer;
    char            target_ssid[BSP_WIFI_SSID_MAX + 1];
} W;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (!W.connected && !W.manual) {
        ESP_LOGI(TAG, "auto-reconnect to %s", W.target_ssid);
        esp_wifi_connect();
    }
}

static void ensure_init(void)
{
    if (W.inited) return;

    setenv("TZ", "CST-8", 1);   /* 显示时区：中国标准时间（时间源见 bsp_time_sync_from_host） */
    tzset();

    /* nvs 可能被写满/版本变更，抹掉重来（KlipperScreen 同款处理） */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_timer_create_args_t t = {.callback = reconnect_cb, .name = "wifi_reconn"};
    esp_timer_create(&t, &W.reconnect_timer);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* 市电供电的显示屏没有省电需求。默认 MIN_MODEM 休眠会让下行包在 AP 侧排队到
       下一个 DTIM，实测 ping RTT 300~600ms 抖动，心跳 RTT 持续爬升并引发
       WS pong 超时——必须关掉（实测关后 ping 降到个位数 ms） */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
    W.inited = true;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        W.record_num = SCAN_MAX;
        esp_wifi_scan_get_ap_records(&W.record_num, W.records);
        W.scan_running = false;
        W.scan_done = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected from %s, reason=%d%s", W.target_ssid, d ? d->reason : -1,
                 W.manual ? " (manual)" : "");
        W.connected = false;
        if (W.manual) {
            /* 主动断开（bsp_wifi_connect 切换 AP 前的 disconnect），不算失败也不重连 */
            W.manual = false;
        } else if (W.state == BSP_WIFI_CONNECTING) {
            W.state = BSP_WIFI_FAILED;      /* 连接中收到断开 = 失败 */
        } else {
            /* 已连接时掉线 → 2s 后自动重连 */
            esp_timer_stop(W.reconnect_timer);
            esp_timer_start_once(W.reconnect_timer, RECONNECT_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        W.state = BSP_WIFI_CONNECTED;
        W.connected = true;
        ESP_LOGI(TAG, "connected, got ip");
    }
}

void bsp_wifi_init(void)
{
    ensure_init();
}

void bsp_wifi_scan_start(void)
{
    ensure_init();
    if (W.scan_running) return;
    W.scan_running = true;
    W.scan_done = false;
    /* 非阻塞扫描，结果由 WIFI_EVENT_SCAN_DONE 带回 */
    esp_wifi_scan_start(NULL, false);
}

int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max)
{
    if (W.scan_running) return BSP_WIFI_SCAN_RUNNING;
    if (!W.scan_done)    return BSP_WIFI_SCAN_FAILED;

    int n = W.record_num < max ? W.record_num : max;
    for (int i = 0; i < n; i++) {
        strlcpy(out[i].ssid, (const char *)W.records[i].ssid, sizeof(out[i].ssid));
        out[i].rssi   = W.records[i].rssi;
        out[i].secure = W.records[i].authmode != WIFI_AUTH_OPEN;
    }
    return n;
}

void bsp_wifi_connect(const char *ssid, const char *password)
{
    ensure_init();
    esp_timer_stop(W.reconnect_timer);
    /* 仅当当前有连接时才主动断开旧 AP（其断开事件用 manual 吞掉，不判失败不重连）。
       无连接时绝不能置 manual——否则首次连接失败的断开事件会被吞，状态卡死 CONNECTING */
    if (W.connected) {
        W.manual = true;
        esp_wifi_disconnect();
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password && password[0])
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    /* PMF(802.11w) 声明支持但不强制：
       WPA2/WPA3 混合模式路由器要求协商 PMF，不支持会导致 4 次握手超时(reason=15) */
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    strlcpy(W.target_ssid, ssid, sizeof(W.target_ssid));
    /* 调试：记录驱动实际收到的 ssid/密码（hex 用来抓隐藏字符、编码问题） */
    ESP_LOGI(TAG, "connect ssid='%s' pass='%s' len=%d", ssid,
             password ? password : "", password ? (int)strlen(password) : 0);
    if (password)
        ESP_LOG_BUFFER_HEX(TAG, password, strlen(password));
    W.state = BSP_WIFI_CONNECTING;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    return W.state;
}

bool bsp_wifi_connected(void)
{
    return W.connected;
}
