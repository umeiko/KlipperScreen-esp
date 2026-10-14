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
#include <stdatomic.h>
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
#define SWITCH_TIMEOUT_US  (3 * 1000 * 1000)
#define CONNECT_TIMEOUT_US (30 * 1000 * 1000)

ESP_EVENT_DEFINE_BASE(BSP_WIFI_CONTROL_EVENT);
enum { WIFI_CONTROL_CONNECT, WIFI_CONTROL_POLL };
typedef enum { WIFI_IDLE, WIFI_STOPPING, WIFI_STARTING,
               WIFI_WAIT_LINK, WIFI_WAIT_IP, WIFI_ONLINE } wifi_phase_t;
typedef struct {
    wifi_config_t config;
    unsigned generation;
} wifi_connect_request_t;

/* Moonraker 与 Bambu Cloud 都提供标准 HTTP Date（GMT），无需额外 NTP
   连接。显示时区沿用项目现有的 CST-8（中国标准时间）。 */
bool bsp_time_sync_from_http_date(const char *http_date)
{
    if (!http_date || !http_date[0]) return false;
    struct tm tmv = {0};
    /* "Wed, 04 Sep 2026 01:35:22 GMT" */
    if (!strptime(http_date, "%a, %d %b %Y %H:%M:%S", &tmv)) return false;
    /* newlib 无 timegm：临时切 UTC 求 epoch 再切回 CST-8 */
    setenv("TZ", "UTC0", 1); tzset();
    time_t t = mktime(&tmv);
    setenv("TZ", "CST-8", 1); tzset();
    if (t <= 0) return false;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return false;
    ESP_LOGI(TAG, "time synced from HTTP Date: %s", http_date);
    return true;
}

static esp_err_t http_date_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key &&
        evt->header_value && strcasecmp(evt->header_key, "Date") == 0)
        bsp_time_sync_from_http_date(evt->header_value);
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
    _Atomic bsp_wifi_state_t state;
    atomic_bool     connected;      /* 仅代表具有可用 IP，不代表驱动尚未关联 */
    atomic_uint     requested_generation;
    atomic_uint     active_generation;
    atomic_bool     request_failed;
    /* 以下连接字段只由默认 event loop 修改；UI / CLI / timer 只投递请求。 */
    wifi_phase_t    phase;
    bool            driver_running;
    bool            auto_reconnect;
    int64_t         deadline_us;
    int64_t         retry_at_us;
    uint8_t         bssid[6];
    wifi_config_t   config;
    esp_timer_handle_t control_timer;
    char            target_ssid[BSP_WIFI_SSID_MAX + 1];
} W;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);

static bool current_request(void)
{
    return W.active_generation == W.requested_generation;
}

static void control_timer_cb(void *arg)
{
    (void)arg;
    /* 队列满时下一节拍重试；不在 esp_timer 线程直接操作连接状态。 */
    esp_event_post(BSP_WIFI_CONTROL_EVENT, WIFI_CONTROL_POLL, NULL, 0, 0);
}

static void connection_failed(const char *operation, esp_err_t err)
{
    ESP_LOGW(TAG, "%s failed: %s", operation, esp_err_to_name(err));
    W.phase = WIFI_IDLE;
    W.deadline_us = 0;
    W.retry_at_us = W.auto_reconnect ? esp_timer_get_time() + RECONNECT_DELAY_US : 0;
    W.connected = false;
    if (current_request()) W.state = BSP_WIFI_FAILED;
}

static void start_configured_wifi(void)
{
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &W.config);
    if (err != ESP_OK) { connection_failed("set config", err); return; }
    W.phase = WIFI_STARTING;
    W.deadline_us = esp_timer_get_time() + SWITCH_TIMEOUT_US;
    err = esp_wifi_start();
    if (err != ESP_OK) { connection_failed("start", err); return; }
    W.driver_running = true;
    /* 等 STA_START 后再 connect；不能让之前排队的断开 / GOT_IP 覆盖新状态。 */
}

static void begin_switch(void)
{
    W.connected = false;
    W.state = BSP_WIFI_CONNECTING;
    W.retry_at_us = 0;
    W.scan_running = false;
    W.scan_done = false;
    if (W.phase == WIFI_STOPPING) return; /* 连点 / CLI 请求合并到最新配置 */
    if (!W.driver_running) { start_configured_wifi(); return; }
    W.phase = WIFI_STOPPING;
    W.deadline_us = esp_timer_get_time() + SWITCH_TIMEOUT_US;
    /* 不凭 GOT_IP 判断是否需要断开：关联中、已关联但 DHCP 未完成也必须取消。
       STA_STOP 是事件屏障；收到它以后才应用新配置，整个过程不阻塞 LVGL。 */
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        W.driver_running = false;
        start_configured_wifi();
    } else if (err != ESP_OK) {
        connection_failed("stop", err);
    }
}

static void ensure_init(void)
{
    if (W.inited) return;

    setenv("TZ", "CST-8", 1);   /* 显示时区：中国标准时间 */
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

    esp_timer_create_args_t t = {
        .callback = control_timer_cb, .name = "wifi_control", .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&t, &W.control_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    ESP_ERROR_CHECK(esp_event_handler_register(BSP_WIFI_CONTROL_EVENT, ESP_EVENT_ANY_ID,
                                             on_wifi_event, NULL));
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* 市电供电的显示屏没有省电需求。默认 MIN_MODEM 休眠会让下行包在 AP 侧排队到
       下一个 DTIM，实测 ping RTT 300~600ms 抖动，心跳 RTT 持续爬升并引发
       WS pong 超时——必须关掉（实测关后 ping 降到个位数 ms） */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_start());
    W.driver_running = true;
    W.inited = true;
    ESP_ERROR_CHECK(esp_timer_start_periodic(W.control_timer, 500000));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == BSP_WIFI_CONTROL_EVENT && id == WIFI_CONTROL_CONNECT) {
        const wifi_connect_request_t *request = data;
        if (!request || request->generation != W.requested_generation) return;
        W.config = request->config;
        W.active_generation = request->generation;
        memcpy(W.target_ssid, W.config.sta.ssid, sizeof(W.config.sta.ssid));
        W.target_ssid[sizeof(W.config.sta.ssid)] = 0;
        W.auto_reconnect = false;
        begin_switch();
    } else if (base == BSP_WIFI_CONTROL_EVENT && id == WIFI_CONTROL_POLL) {
        if (!current_request()) return;
        int64_t now = esp_timer_get_time();
        if (W.deadline_us && now >= W.deadline_us) {
            connection_failed("connection timeout", ESP_ERR_TIMEOUT);
            /* 超时后也停止悬而未决的关联 / DHCP，迟到的 IP 不再视作成功。 */
            esp_err_t err = esp_wifi_disconnect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED)
                ESP_LOGW(TAG, "timeout disconnect failed: %s", esp_err_to_name(err));
        } else if (W.retry_at_us && now >= W.retry_at_us) {
            ESP_LOGI(TAG, "auto-reconnect to %s", W.target_ssid);
            begin_switch();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        W.driver_running = false;
        W.connected = false;
        if (W.phase == WIFI_STOPPING) {
            if (current_request()) start_configured_wifi();
            else { W.phase = WIFI_IDLE; W.deadline_us = 0; }
        } else {
            /* CLI wifioff 等外部停止不应触发自动重启。 */
            W.auto_reconnect = false;
            connection_failed("driver stopped", ESP_ERR_WIFI_NOT_STARTED);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        W.driver_running = true;
        if (W.phase != WIFI_STARTING || !current_request()) return;
        W.phase = WIFI_WAIT_LINK;
        W.deadline_us = esp_timer_get_time() + CONNECT_TIMEOUT_US;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) connection_failed("connect", err);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ap = data;
        size_t len = strlen(W.target_ssid);
        if (W.phase != WIFI_WAIT_LINK || !current_request() || !ap ||
            ap->ssid_len != len || memcmp(ap->ssid, W.target_ssid, len) != 0) return;
        memcpy(W.bssid, ap->bssid, sizeof(W.bssid));
        W.phase = WIFI_WAIT_IP; /* 关联成功不等于 DHCP 成功，仍保留连接期限。 */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (!W.scan_running) return; /* 切网时已取消的扫描结果 */
        W.record_num = SCAN_MAX;
        esp_err_t err = esp_wifi_scan_get_ap_records(&W.record_num, W.records);
        W.scan_running = false;
        W.scan_done = err == ESP_OK;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (!current_request() || W.phase == WIFI_STOPPING || W.phase == WIFI_STARTING ||
            W.phase == WIFI_IDLE) return;
        size_t len = strlen(W.target_ssid);
        if (d && d->ssid_len && (d->ssid_len != len ||
            memcmp(d->ssid, W.target_ssid, len) != 0)) return;
        ESP_LOGW(TAG, "disconnected from %s, reason=%d", W.target_ssid, d ? d->reason : -1);
        connection_failed("association disconnected", ESP_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* 旧连接排队的 GOT_IP 不能结束新连接弹层；同 SSID 重试也核对当前 BSSID。 */
        if (!current_request() || W.phase != WIFI_WAIT_IP) return;
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
            strncmp((char *)ap.ssid, W.target_ssid, sizeof(W.config.sta.ssid)) != 0 ||
            memcmp(ap.bssid, W.bssid, sizeof(W.bssid)) != 0) return;
        W.phase = WIFI_ONLINE;
        W.deadline_us = W.retry_at_us = 0;
        W.auto_reconnect = true;
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
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        W.scan_running = false;
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
    }
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
    wifi_connect_request_t request = {0};
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t pass_len = password ? strlen(password) : 0;
    W.request_failed = false;
    request.generation = atomic_fetch_add(&W.requested_generation, 1) + 1;
    W.connected = false;
    W.state = BSP_WIFI_CONNECTING;
    if (!ssid_len || ssid_len > BSP_WIFI_SSID_MAX || pass_len > BSP_WIFI_PASS_MAX) {
        W.request_failed = true;
        W.state = BSP_WIFI_FAILED;
        ESP_LOGW(TAG, "invalid SSID/password length");
        return;
    }
    wifi_config_t *cfg = &request.config;
    /* STA 的 32 字节 SSID 可没有 NUL；strlcpy 会错误截短最大长度的 SSID。 */
    memcpy(cfg->sta.ssid, ssid, ssid_len);
    if (password && password[0])
        strlcpy((char *)cfg->sta.password, password, sizeof(cfg->sta.password));
    cfg->sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    /* PMF(802.11w) 声明支持但不强制：
       WPA2/WPA3 混合模式路由器要求协商 PMF，不支持会导致 4 次握手超时(reason=15) */
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "connect ssid='%s', password length=%u", ssid, (unsigned)pass_len);
    /* 按值投递，密码不依赖调用方栈 / textarea 的生命周期。 */
    esp_err_t err = esp_event_post(BSP_WIFI_CONTROL_EVENT, WIFI_CONTROL_CONNECT,
                                  &request, sizeof(request), 0);
    if (err != ESP_OK) {
        W.request_failed = true;
        W.state = BSP_WIFI_FAILED;
        ESP_LOGW(TAG, "queue connect failed: %s", esp_err_to_name(err));
    }
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    if (!current_request())
        return W.request_failed ? BSP_WIFI_FAILED : BSP_WIFI_CONNECTING;
    return W.state;
}

bool bsp_wifi_connected(void)
{
    return current_request() && W.connected;
}

bool bsp_wifi_current(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    if (ssid && ssid_len) ssid[0] = 0;
    if (ip && ip_len) ip[0] = 0;
    if (!bsp_wifi_connected()) return false;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid && ssid_len) strlcpy(ssid, (const char *)ap.ssid, ssid_len);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && ip && ip_len) {
        esp_netif_ip_info_t ii;
        if (esp_netif_get_ip_info(sta, &ii) == ESP_OK && ii.ip.addr != 0)
            snprintf(ip, ip_len, IPSTR, IP2STR(&ii.ip));
    }
    return true;
}
