/*
 * ESP32 Bambu Cloud read-only monitor.
 *
 * Network ownership belongs to the bambu_net actor in bambu_runtime_esp32.c.
 * ESP-MQTT has its own protocol task, but callbacks only update fixed-size
 * state/feed the streaming parser and wake bambu_net.  They never stop a
 * client, perform HTTP, allocate a report buffer, or touch LVGL.
 */
#include "bambu_monitor.h"
#include "bambu_monitor_esp32_internal.h"
#include "bambu_runtime_esp32.h"

#include "bambu_status_stream.h"
#include "bsp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "mbedtls/ssl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bambu_mqtt";

#define MQTT_TOKEN_MAX          2048
#define MQTT_USER_MAX             96
#define MQTT_SERIAL_MAX           40
#define MQTT_TOPIC_MAX            72
#define MQTT_CLIENT_ID_MAX        32
#define MQTT_RX_BUFFER           4096
#define MQTT_TX_BUFFER           1024
#define MQTT_TASK_STACK          6144
#define MQTT_PUSHALL_GAP_MS    300000ULL

static const char PUSHALL[] =
    "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\","
    "\"version\":1,\"push_target\":1}}";

typedef struct mqtt_session {
    uint32_t generation;
    bambu_cloud_region_t region;
    char serial[MQTT_SERIAL_MAX];
    char user_id[MQTT_USER_MAX];
    char token[MQTT_TOKEN_MAX];
    char host[32];
    char report_topic[MQTT_TOPIC_MAX];
    char request_topic[MQTT_TOPIC_MAX];
    char client_id[MQTT_CLIENT_ID_MAX];

    esp_mqtt_client_handle_t client;
    bool started;
    bool closing;

    /* Callback -> actor event mailbox, guarded by g_mutex. */
    bool mqtt_connected;
    bool ev_connected;
    bool ev_disconnected;
    bool ev_auth_error;
    bool ev_network_error;
    int ev_subscribed_id;

    /* Actor-owned protocol state. */
    bool subscribe_requested;
    bool subscribed;
    bool pushall_sent;
    int subscribe_id;

    /* Callback-owned single-message streaming state. */
    bambu_status_stream_t parser;
    size_t rx_total;
    size_t rx_received;
    unsigned rx_fragments;
    bool rx_active;
    bool rx_drop;
    bool first_report_logged;
    bool fragmented_report_logged;
} mqtt_session_t;

static StaticSemaphore_t g_mutex_buf;
static SemaphoreHandle_t g_mutex;
static bool g_initialized;
static bool g_enabled;
static bool g_paused;
static uint32_t g_generation = 1;
static uint32_t g_auth_block_generation;
static char g_target[MQTT_SERIAL_MAX];
static bambu_monitor_snapshot_t g_snapshot;
static mqtt_session_t *g_session;
static uint64_t g_retry_at_ms;
static unsigned g_retry_step;
static char g_last_push_serial[MQTT_SERIAL_MAX];
static uint64_t g_last_push_ms;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

/* CYD deliberately uses a 4 KiB mbedTLS input buffer: restoring the global
   16 KiB default makes the HTTPS login handshake compete with LVGL for the
   last few tens of KiB.  A smaller compile-time buffer is only valid when the
   peer is told to split records to match it.  The Bambu MQTT brokers support
   RFC 6066 Maximum Fragment Length, so add that per MQTT connection while
   retaining the normal certificate-bundle verification. */
static esp_err_t mqtt_tls_attach(void *conf)
{
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err != ESP_OK) return err;
    if (!conf || mbedtls_ssl_conf_max_frag_len(
                     (mbedtls_ssl_config *)conf,
                     MBEDTLS_SSL_MAX_FRAG_LEN_4096) != 0)
        return ESP_FAIL;
    return ESP_OK;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *serial_tail(const char *serial)
{
    size_t n = serial ? strlen(serial) : 0;
    return n > 4 ? serial + n - 4 : (serial ? serial : "");
}

static void initialize_once(void)
{
    if (g_initialized) return;
    if (!g_mutex) g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_buf);
    if (!g_mutex) return;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
    g_initialized = true;
}

static bool session_alive_locked(const mqtt_session_t *s)
{
    return s && s == g_session && !s->closing && g_enabled && !g_paused &&
           s->generation == g_generation &&
           strcmp(s->serial, g_target) == 0;
}

static void publish_connection(uint32_t generation,
                               bambu_monitor_state_t state,
                               bool connected, const char *message)
{
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (generation == g_generation && g_enabled) {
        g_snapshot.state = state;
        g_snapshot.connected = connected;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    }
    xSemaphoreGive(g_mutex);
}

static bool topic_matches(const esp_mqtt_event_handle_t event,
                          const char *expected)
{
    size_t n = strlen(expected);
    return event->topic && event->topic_len >= 0 &&
           (size_t)event->topic_len == n &&
           memcmp(event->topic, expected, n) == 0;
}

static bool callback_session_alive(mqtt_session_t *s)
{
    bool alive;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    alive = session_alive_locked(s);
    xSemaphoreGive(g_mutex);
    return alive;
}

static void handle_report_data(mqtt_session_t *s,
                               esp_mqtt_event_handle_t event)
{
    if (!callback_session_alive(s)) return;

    const int total_i = event->total_data_len;
    const int offset_i = event->current_data_offset;
    const int len_i = event->data_len;
    if (total_i <= 0 || offset_i < 0 || len_i < 0) {
        s->rx_active = false;
        s->rx_drop = true;
        return;
    }

    const size_t total = (size_t)total_i;
    const size_t offset = (size_t)offset_i;
    const size_t len = (size_t)len_i;

    if (offset == 0) {
        s->rx_active = false;
        s->rx_drop = true;
        s->rx_total = total;
        s->rx_received = 0;
        s->rx_fragments = 0;
        if (total > BAMBUSTREAM_MAX_TOTAL || len > total ||
            !topic_matches(event, s->report_topic))
            return;
        bambu_status_stream_begin(&s->parser, total);
        s->rx_active = true;
        s->rx_drop = false;
    } else {
        /* ESP-MQTT normally supplies the topic only on the first fragment.  If
           it supplies one later, it must still be the exact target topic. */
        if (!s->rx_active || s->rx_drop || total != s->rx_total ||
            offset > total || offset != s->rx_received ||
            len > total - offset ||
            (event->topic_len > 0 && !topic_matches(event, s->report_topic))) {
            s->rx_active = false;
            s->rx_drop = true;
            return;
        }
    }

    if (!s->rx_active || offset > total || offset != s->rx_received ||
        len > total - offset ||
        !bambu_status_stream_feed(&s->parser, event->data, len)) {
        s->rx_active = false;
        s->rx_drop = true;
        return;
    }
    s->rx_received += len;
    s->rx_fragments++;
    if (s->rx_received != s->rx_total) return;

    bambu_status_t candidate;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    candidate = g_snapshot.printer;
    xSemaphoreGive(g_mutex);

    bool parsed = bambu_status_stream_finish(&s->parser, &candidate);
    s->rx_active = false;
    if (s->rx_fragments > 1 && !s->fragmented_report_logged) {
        s->fragmented_report_logged = true;
        ESP_LOGI(TAG, "fragmented report: total=%u fragments=%u parsed=%d",
                 (unsigned)s->rx_total, s->rx_fragments, parsed);
    }
    if (!parsed) {
        /* The report topic also carries heartbeats, camera/AMS notices and
           other valid JSON without any field represented by bambu_status_t.
           Ignoring those is normal and must not flood the warning log. */
        s->rx_drop = true;
        return;
    }

    bool published = false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (session_alive_locked(s)) {
        g_snapshot.printer = candidate;
        g_snapshot.state = BAMBU_MONITOR_CONNECTED;
        g_snapshot.connected = true;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "实时状态已同步");
        published = true;
    }
    xSemaphoreGive(g_mutex);
    if (published && !s->first_report_logged) {
        s->first_report_logged = true;
        ESP_LOGI(TAG, "report ok: serial=...%s total=%u fragments=%u",
                 serial_tail(s->serial), (unsigned)s->rx_total,
                 s->rx_fragments);
    }
}

static void mqtt_event_handler(void *handler_arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)base;
    mqtt_session_t *s = (mqtt_session_t *)handler_arg;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (!s || !event) return;

    if (event_id == MQTT_EVENT_DATA) {
        handle_report_data(s, event);
        return;
    }

    bool wake = false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (session_alive_locked(s)) {
        switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s->mqtt_connected = true;
            s->ev_connected = true;
            wake = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            s->mqtt_connected = false;
            s->ev_disconnected = true;
            wake = true;
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (event->error_handle &&
                event->error_handle->error_type ==
                    MQTT_ERROR_TYPE_SUBSCRIBE_FAILED)
                s->ev_network_error = true;
            else
                s->ev_subscribed_id = event->msg_id;
            wake = true;
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle &&
                event->error_handle->error_type ==
                    MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (event->error_handle->connect_return_code ==
                     MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                 event->error_handle->connect_return_code ==
                     MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED))
                s->ev_auth_error = true;
            else
                s->ev_network_error = true;
            wake = true;
            break;
        default:
            break;
        }
    }
    xSemaphoreGive(g_mutex);
    if (wake) bambu_rt_wake();
}

static void schedule_retry(uint32_t generation, uint64_t now)
{
    static const unsigned delay_s[] = {1, 2, 4, 8, 15};
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (generation != g_generation || !g_enabled) {
        xSemaphoreGive(g_mutex);
        return;
    }
    unsigned index = g_retry_step;
    if (index >= sizeof(delay_s) / sizeof(delay_s[0]))
        index = sizeof(delay_s) / sizeof(delay_s[0]) - 1;
    g_retry_at_ms = now + (uint64_t)delay_s[index] * 1000ULL;
    if (g_retry_step + 1 < sizeof(delay_s) / sizeof(delay_s[0]))
        g_retry_step++;
    xSemaphoreGive(g_mutex);
}

static void destroy_session(mqtt_session_t *s)
{
    if (!s) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    s->closing = true;
    if (g_session == s) g_session = NULL;
    xSemaphoreGive(g_mutex);

    if (s->client) {
        if (s->started) esp_mqtt_client_stop(s->client);
        esp_mqtt_client_destroy(s->client);
    }
    ESP_LOGI(TAG, "client stopped: serial=...%s", serial_tail(s->serial));
    bambu_rt_wipe(s, sizeof(*s));
    free(s);
}

static mqtt_session_t *new_session(uint32_t generation, const char *serial,
                                   bool *credentials_missing)
{
    if (credentials_missing) *credentials_missing = false;
    mqtt_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->generation = generation;
    copy_text(s->serial, sizeof(s->serial), serial);
    if (!bambu_rt_copy_mqtt_credentials(&s->region,
                                         s->user_id, sizeof(s->user_id),
                                         s->token, sizeof(s->token))) {
        if (credentials_missing) *credentials_missing = true;
        bambu_rt_wipe(s, sizeof(*s));
        free(s);
        return NULL;
    }

    copy_text(s->host, sizeof(s->host),
              s->region == BAMBU_CLOUD_REGION_CHINA
                  ? "cn.mqtt.bambulab.com" : "us.mqtt.bambulab.com");
    snprintf(s->report_topic, sizeof(s->report_topic),
             "device/%s/report", s->serial);
    snprintf(s->request_topic, sizeof(s->request_topic),
             "device/%s/request", s->serial);
    snprintf(s->client_id, sizeof(s->client_id), "bblp_%08lx",
             (unsigned long)esp_random());

    esp_mqtt_client_config_t config = {
        .broker = {
            .address = {
                .hostname = s->host,
                .transport = MQTT_TRANSPORT_OVER_SSL,
                .port = 8883,
            },
            .verification = {
                .crt_bundle_attach = mqtt_tls_attach,
                .skip_cert_common_name_check = false,
            },
        },
        .credentials = {
            .username = s->user_id,
            .client_id = s->client_id,
            .authentication = {.password = s->token},
        },
        .session = {
            .keepalive = 60,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .timeout_ms = 10000,
            .disable_auto_reconnect = true,
        },
        .task = {
            .priority = tskIDLE_PRIORITY + 2,
            .stack_size = MQTT_TASK_STACK,
        },
        .buffer = {
            .size = MQTT_RX_BUFFER,
            .out_size = MQTT_TX_BUFFER,
        },
        .outbox = {.limit = 2048},
    };
    s->client = esp_mqtt_client_init(&config);
    if (!s->client ||
        esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY,
                                       mqtt_event_handler, s) != ESP_OK) {
        if (s->client) esp_mqtt_client_destroy(s->client);
        bambu_rt_wipe(s, sizeof(*s));
        free(s);
        return NULL;
    }
    return s;
}

static bool attach_and_start(mqtt_session_t *s)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool current = g_enabled && !g_paused && !g_session &&
                   s->generation == g_generation &&
                   strcmp(s->serial, g_target) == 0;
    if (current) g_session = s;
    xSemaphoreGive(g_mutex);
    if (!current) return false;

    esp_err_t err = esp_mqtt_client_start(s->client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "client start failed: %s", esp_err_to_name(err));
        return false;
    }
    s->started = true;
    ESP_LOGI(TAG, "client started: serial=...%s heap=%uB",
             serial_tail(s->serial),
             (unsigned)esp_get_free_heap_size());
    return true;
}

void bambu_monitor_init(void)
{
    initialize_once();
    bambu_rt_init();
}

void bambu_monitor_start(const char *serial)
{
    bambu_monitor_init();
    if (!serial || !serial[0]) {
        bambu_monitor_stop();
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_enabled && strcmp(g_target, serial) == 0) {
        xSemaphoreGive(g_mutex);
        return;
    }
    g_generation++;
    g_enabled = true;
    g_paused = false;
    copy_text(g_target, sizeof(g_target), serial);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_CONNECTING;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message),
              "正在连接拓竹实时状态…");
    bambu_status_reset(&g_snapshot.printer);
    g_auth_block_generation = 0;
    g_retry_at_ms = 0;
    g_retry_step = 0;
    xSemaphoreGive(g_mutex);
    bambu_rt_wake();
}

void bambu_monitor_stop(void)
{
    initialize_once();
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (!g_enabled && g_snapshot.state == BAMBU_MONITOR_STOPPED) {
        xSemaphoreGive(g_mutex);
        return;
    }
    g_generation++;
    g_enabled = false;
    g_paused = false;
    g_target[0] = 0;
    g_auth_block_generation = 0;
    g_retry_at_ms = 0;
    g_retry_step = 0;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
    xSemaphoreGive(g_mutex);
    bambu_rt_wake();
}

void bambu_monitor_snapshot(bambu_monitor_snapshot_t *out)
{
    if (!out) return;
    bambu_monitor_init();
    if (!g_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    *out = g_snapshot;
    xSemaphoreGive(g_mutex);
}

void bambu_monitor_actor_suspend(void)
{
    initialize_once();
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_paused = true;
    mqtt_session_t *s = g_session;
    if (s) {
        s->closing = true;
        g_session = NULL;
    }
    if (g_enabled) {
        g_snapshot.connected = false;
        g_snapshot.state = BAMBU_MONITOR_CONNECTING;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "云端操作完成后恢复实时状态…");
    }
    xSemaphoreGive(g_mutex);
    destroy_session(s);
}

void bambu_monitor_actor_resume(void)
{
    initialize_once();
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_paused = false;
    if (g_enabled) {
        g_retry_at_ms = 0;
        g_retry_step = 0;
    }
    xSemaphoreGive(g_mutex);
    bambu_rt_wake();
}

bool bambu_monitor_actor_needs_tick(void)
{
    initialize_once();
    if (!g_mutex) return false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool needs = g_session ||
                 (g_enabled && !g_paused &&
                  g_auth_block_generation != g_generation);
    xSemaphoreGive(g_mutex);
    return needs;
}

void bambu_monitor_actor_tick(void)
{
    initialize_once();
    if (!g_mutex) return;

    uint32_t generation;
    char target[MQTT_SERIAL_MAX];
    mqtt_session_t *s;
    bool enabled;
    bool paused;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    generation = g_generation;
    copy_text(target, sizeof(target), g_target);
    enabled = g_enabled;
    paused = g_paused;
    s = g_session;
    xSemaphoreGive(g_mutex);

    if (!enabled || paused) {
        if (s) destroy_session(s);
        return;
    }
    if (s && !callback_session_alive(s)) {
        destroy_session(s);
        s = NULL;
    }

    const uint64_t now = now_ms();
    if (!bsp_wifi_connected()) {
        if (s) destroy_session(s);
        publish_connection(generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                           "Wi-Fi 未连接，等待恢复…");
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (generation == g_generation) g_retry_at_ms = now + 1000;
        xSemaphoreGive(g_mutex);
        return;
    }

    if (s) {
        bool connected, disconnected, auth_error, network_error;
        int subscribed_id;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        connected = s->ev_connected;
        disconnected = s->ev_disconnected;
        auth_error = s->ev_auth_error;
        network_error = s->ev_network_error;
        subscribed_id = s->ev_subscribed_id;
        s->ev_connected = false;
        s->ev_disconnected = false;
        s->ev_auth_error = false;
        s->ev_network_error = false;
        s->ev_subscribed_id = 0;
        xSemaphoreGive(g_mutex);

        if (auth_error) {
            destroy_session(s);
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_auth_block_generation = generation;
            xSemaphoreGive(g_mutex);
            publish_connection(generation, BAMBU_MONITOR_AUTH_ERROR, false,
                               "云端实时连接被拒绝，请重新登录");
            return;
        }
        if (network_error || disconnected) {
            destroy_session(s);
            publish_connection(generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                               "实时连接中断，正在重试…");
            schedule_retry(generation, now);
            return;
        }
        if (connected) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (generation == g_generation) {
                g_retry_step = 0;
                g_retry_at_ms = 0;
            }
            xSemaphoreGive(g_mutex);
            publish_connection(generation, BAMBU_MONITOR_CONNECTED, true,
                               "已连接，正在同步打印机状态…");
        }
        if (s->mqtt_connected && !s->subscribe_requested) {
            int id = esp_mqtt_client_subscribe(s->client,
                                                s->report_topic, 0);
            if (id < 0) {
                destroy_session(s);
                publish_connection(generation,
                                   BAMBU_MONITOR_NETWORK_ERROR, false,
                                   "订阅实时状态失败，正在重试…");
                schedule_retry(generation, now);
                return;
            }
            s->subscribe_id = id;
            s->subscribe_requested = true;
        }
        if (subscribed_id && s->subscribe_requested &&
            subscribed_id == s->subscribe_id)
            s->subscribed = true;

        if (s->subscribed && !s->pushall_sent) {
            bool same = strcmp(g_last_push_serial, s->serial) == 0;
            bool allowed = !same || !g_last_push_ms ||
                           now - g_last_push_ms >= MQTT_PUSHALL_GAP_MS;
            if (allowed) {
                int id = esp_mqtt_client_publish(s->client, s->request_topic,
                                                 PUSHALL, sizeof(PUSHALL) - 1,
                                                 0, 0);
                if (id >= 0) {
                    s->pushall_sent = true;
                    copy_text(g_last_push_serial,
                              sizeof(g_last_push_serial), s->serial);
                    g_last_push_ms = now;
                    ESP_LOGI(TAG, "pushall sent: serial=...%s",
                             serial_tail(s->serial));
                }
            }
        }
        return;
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool auth_blocked = g_auth_block_generation == generation;
    xSemaphoreGive(g_mutex);
    uint64_t retry_at;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    retry_at = g_retry_at_ms;
    xSemaphoreGive(g_mutex);
    if (auth_blocked || (retry_at && now < retry_at)) return;

    bool credentials_missing = false;
    s = new_session(generation, target, &credentials_missing);
    if (!s) {
        if (credentials_missing) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_auth_block_generation = generation;
            xSemaphoreGive(g_mutex);
            publish_connection(generation, BAMBU_MONITOR_AUTH_ERROR, false,
                               "登录信息不完整，请退出后重新登录");
        } else {
            publish_connection(generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                               "内存不足，实时状态稍后重试…");
            schedule_retry(generation, now);
        }
        return;
    }
    publish_connection(generation, BAMBU_MONITOR_CONNECTING, false,
                       "正在连接拓竹实时状态…");
    if (!attach_and_start(s)) {
        destroy_session(s);
        publish_connection(generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                           "无法启动实时状态连接，正在重试…");
        schedule_retry(generation, now);
    }
}
