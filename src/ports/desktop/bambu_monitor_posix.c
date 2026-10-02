/*
 * Bambu Cloud MQTT 实时状态监视（POSIX 后端：Linux 桌面 / Android）。
 * 与 Windows 的 bambu_monitor_openssl.c 同一协议逻辑：
 * cn/us.mqtt.bambulab.com:8883（TLS），MQTT 3.1.1，user_id/token 鉴权，
 * 订阅 device/<serial>/report，发 pushall 拉全量，30s ping，断线指数退避重连。
 * TLS/网络差异收进 bambu_tls 层；状态解析复用共享的 bambu_status_*。
 * 线程模型：每路会话一个 pthread worker，快照经 g_lock 互斥发布，
 * worker 永不触碰 LVGL（桌面端 LV_USE_OS=NONE，UI 侧只读快照）。
 */
#include "bambu_monitor.h"
#include "bambu_cloud_internal.h"
#include "bambu_tls.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MQTT_PORT          "8883"
#define MQTT_TOKEN_MAX       2048
#define MQTT_PACKET_MAX  (64 * 1024)

typedef struct {
    int generation;
    bambu_cloud_region_t region;
    char serial[40];
    char user_id[96];
    char token[MQTT_TOKEN_MAX];
} monitor_args_t;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock;
static bambu_monitor_snapshot_t g_snapshot;
static atomic_int g_generation = 1;
static bool g_enabled;
static char g_target[40];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void wipe(void *p, size_t n) { memset(p, 0, n); }

static void once_init(void)
{
    pthread_mutex_init(&g_lock, NULL);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
}

void bambu_monitor_init(void)
{
    pthread_once(&g_once, once_init);
}

static bool generation_alive(int generation)
{
    return atomic_load(&g_generation) == generation;
}

static void publish_connection(int generation, bambu_monitor_state_t state,
                               bool connected, const char *message)
{
    if (!generation_alive(generation)) return;
    pthread_mutex_lock(&g_lock);
    g_snapshot.state = state;
    g_snapshot.connected = connected;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    pthread_mutex_unlock(&g_lock);
}

/* 1=byte read, 0=one-second receive timeout, -1=connection closed/error. */
static int tls_read_byte(bambu_tls_t *tls, unsigned char *out)
{
    return bambu_tls_read(tls, out, 1);
}

static int tls_read_exact(bambu_tls_t *tls, unsigned char *out, size_t len,
                          int generation)
{
    size_t done = 0;
    int idle = 0;
    while (done < len && generation_alive(generation)) {
        int n = bambu_tls_read(tls, out + done, (int)(len - done));
        if (n > 0) { done += (size_t)n; idle = 0; continue; }
        if (n == 0 && ++idle < 8) continue;
        return -1;
    }
    return done == len ? 0 : -1;
}

static size_t encode_remaining(unsigned char out[4], size_t value)
{
    size_t n = 0;
    do {
        unsigned char b = (unsigned char)(value % 128u);
        value /= 128u;
        if (value) b |= 0x80;
        out[n++] = b;
    } while (value && n < 4);
    return n;
}

static int mqtt_send(bambu_tls_t *tls, unsigned char type,
                     const unsigned char *body, size_t body_len)
{
    unsigned char head[5];
    head[0] = type;
    size_t n = encode_remaining(head + 1, body_len);
    if (bambu_tls_write_all(tls, head, (int)(n + 1)) != 0) return -1;
    return body_len ? bambu_tls_write_all(tls, body, (int)body_len) : 0;
}

static bool append_utf8(unsigned char *buf, size_t cap, size_t *used,
                        const char *value)
{
    size_t n = strlen(value);
    if (n > 65535 || *used + n + 2 > cap) return false;
    buf[(*used)++] = (unsigned char)(n >> 8);
    buf[(*used)++] = (unsigned char)n;
    memcpy(buf + *used, value, n);
    *used += n;
    return true;
}

static int mqtt_connect_packet(bambu_tls_t *tls, const char *client_id,
                               const char *user, const char *password)
{
    size_t cap = strlen(user) + strlen(password) + strlen(client_id) + 32;
    unsigned char *body = malloc(cap);
    if (!body) return -1;
    size_t used = 0;
    static const unsigned char variable[] = {
        0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 60
    };
    memcpy(body, variable, sizeof(variable));
    used = sizeof(variable);
    bool ok = append_utf8(body, cap, &used, client_id) &&
              append_utf8(body, cap, &used, user) &&
              append_utf8(body, cap, &used, password);
    int rc = ok ? mqtt_send(tls, 0x10, body, used) : -1;
    wipe(body, cap);
    free(body);
    return rc;
}

/* 1=packet, 0=idle timeout, -1=disconnect/protocol error. */
static int mqtt_read_packet(bambu_tls_t *tls, unsigned char *type,
                            unsigned char **body, size_t *body_len,
                            int generation)
{
    unsigned char fixed;
    int first = tls_read_byte(tls, &fixed);
    if (first <= 0) return first;
    size_t remaining = 0, multiplier = 1;
    for (int i = 0; i < 4; i++) {
        unsigned char b;
        if (tls_read_exact(tls, &b, 1, generation) != 0) return -1;
        remaining += (size_t)(b & 0x7f) * multiplier;
        if (!(b & 0x80)) break;
        multiplier *= 128;
        if (i == 3) return -1;
    }
    if (remaining > MQTT_PACKET_MAX) return -1;
    unsigned char *payload = malloc(remaining + 1);
    if (!payload) return -1;
    if (remaining && tls_read_exact(tls, payload, remaining, generation) != 0) {
        free(payload);
        return -1;
    }
    payload[remaining] = 0;
    *type = fixed;
    *body = payload;
    *body_len = remaining;
    return 1;
}

static int wait_connack(bambu_tls_t *tls, int generation)
{
    for (int tries = 0; tries < 10 && generation_alive(generation); tries++) {
        unsigned char type, *body = NULL;
        size_t len = 0;
        int rc = mqtt_read_packet(tls, &type, &body, &len, generation);
        if (rc == 0) continue;
        if (rc < 0) return -1;
        int result = ((type >> 4) == 2 && len >= 2) ? body[1] : -2;
        free(body);
        if (result != -2) return result;
    }
    return -1;
}

static int mqtt_subscribe(bambu_tls_t *tls, const char *topic)
{
    unsigned char body[160];
    size_t used = 0;
    body[used++] = 0;
    body[used++] = 1;
    if (!append_utf8(body, sizeof(body), &used, topic)) return -1;
    body[used++] = 0; /* QoS 0 */
    return mqtt_send(tls, 0x82, body, used);
}

static int mqtt_publish(bambu_tls_t *tls, const char *topic, const char *json)
{
    size_t cap = strlen(topic) + strlen(json) + 3;
    unsigned char *body = malloc(cap);
    if (!body) return -1;
    size_t used = 0;
    bool ok = append_utf8(body, cap, &used, topic);
    if (ok) {
        size_t n = strlen(json);
        memcpy(body + used, json, n);
        used += n;
    }
    int rc = ok ? mqtt_send(tls, 0x30, body, used) : -1;
    free(body);
    return rc;
}

static void merge_publish(int generation, unsigned char fixed,
                          const unsigned char *body, size_t len,
                          const char *expected_topic)
{
    if (len < 2 || !generation_alive(generation)) return;
    size_t topic_len = ((size_t)body[0] << 8) | body[1];
    size_t pos = topic_len + 2;
    if (pos > len || topic_len != strlen(expected_topic) ||
        memcmp(body + 2, expected_topic, topic_len) != 0) return;
    unsigned qos = (fixed >> 1) & 3;
    if (qos) pos += 2;
    if (pos > len) return;

    pthread_mutex_lock(&g_lock);
    if (generation_alive(generation) &&
        bambu_status_apply_json(&g_snapshot.printer,
                                (const char *)body + pos, len - pos)) {
        g_snapshot.connected = true;
        g_snapshot.state = BAMBU_MONITOR_CONNECTED;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), "实时状态已同步");
    }
    pthread_mutex_unlock(&g_lock);
}

/* 0=normal disconnect, 4/5=MQTT auth failure, -1=network/protocol error. */
static int run_session(monitor_args_t *args)
{
    const char *host = args->region == BAMBU_CLOUD_REGION_CHINA
                     ? "cn.mqtt.bambulab.com" : "us.mqtt.bambulab.com";
    bambu_tls_t *tls = bambu_tls_connect(host, MQTT_PORT, 6000);
    if (!tls) return -1;

    unsigned char random[6] = {0};
    srand((unsigned)(now_ms() ^ (uint64_t)(uintptr_t)&tls));
    for (unsigned i = 0; i < sizeof(random); i++) random[i] = (unsigned char)rand();
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "bblp_%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3], random[4], random[5]);
    if (mqtt_connect_packet(tls, client_id, args->user_id, args->token) != 0) {
        bambu_tls_close(tls); return -1;
    }
    int ack = wait_connack(tls, args->generation);
    if (ack != 0) { bambu_tls_close(tls); return ack; }

    char report[96], request[96];
    snprintf(report, sizeof(report), "device/%s/report", args->serial);
    snprintf(request, sizeof(request), "device/%s/request", args->serial);
    if (mqtt_subscribe(tls, report) != 0 ||
        mqtt_publish(tls, request,
            "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}") != 0) {
        bambu_tls_close(tls); return -1;
    }
    publish_connection(args->generation, BAMBU_MONITOR_CONNECTED, true,
                       "已连接，正在读取打印机状态…");

    uint64_t last_tx = now_ms();
    while (generation_alive(args->generation)) {
        unsigned char type, *body = NULL;
        size_t len = 0;
        int rc = mqtt_read_packet(tls, &type, &body, &len, args->generation);
        if (rc < 0) break;
        if (rc > 0) {
            if ((type >> 4) == 3) merge_publish(args->generation, type, body, len, report);
            free(body);
        }
        uint64_t now = now_ms();
        if (now - last_tx >= 30000) {
            if (mqtt_send(tls, 0xC0, NULL, 0) != 0) break;
            last_tx = now;
        }
    }
    bambu_tls_close(tls);
    return 0;
}

static void *monitor_worker(void *context)
{
    monitor_args_t *args = context;
    if (!args->user_id[0] &&
        !bambu_cloud_resolve_mqtt_user_id(args->token, args->region,
                                          args->user_id, sizeof(args->user_id))) {
        publish_connection(args->generation, BAMBU_MONITOR_AUTH_ERROR, false,
                           "无法取得云端实时监视身份，请重新登录");
        wipe(args->token, sizeof(args->token));
        free(args);
        return NULL;
    }
    unsigned backoff_ms = 1000;
    while (generation_alive(args->generation)) {
        publish_connection(args->generation, BAMBU_MONITOR_CONNECTING, false,
                           "正在连接拓竹实时状态…");
        int rc = run_session(args);
        if (!generation_alive(args->generation)) break;
        if (rc == 4 || rc == 5) {
            publish_connection(args->generation, BAMBU_MONITOR_AUTH_ERROR, false,
                               "云端实时连接被拒绝，请重新登录");
            break;
        }
        publish_connection(args->generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                           "实时连接中断，正在重试…");
        unsigned waited = 0;
        while (waited < backoff_ms && generation_alive(args->generation)) {
            sleep_ms(100);
            waited += 100;
        }
        if (backoff_ms < 15000) backoff_ms *= 2;
        if (backoff_ms > 15000) backoff_ms = 15000;
    }
    wipe(args->token, sizeof(args->token));
    wipe(args->user_id, sizeof(args->user_id));
    free(args);
    return NULL;
}

void bambu_monitor_start(const char *serial)
{
    bambu_monitor_init();
    if (!serial || !serial[0]) { bambu_monitor_stop(); return; }
    pthread_mutex_lock(&g_lock);
    bool same = g_enabled && strcmp(g_target, serial) == 0;
    pthread_mutex_unlock(&g_lock);
    if (same) return;

    monitor_args_t *args = calloc(1, sizeof(*args));
    if (!args) return;
    copy_text(args->serial, sizeof(args->serial), serial);
    if (!bambu_cloud_copy_mqtt_credentials(args->user_id, sizeof(args->user_id),
                                            args->token, sizeof(args->token),
                                            &args->region)) {
        wipe(args, sizeof(*args));
        free(args);
        int generation = atomic_fetch_add(&g_generation, 1) + 1;
        pthread_mutex_lock(&g_lock);
        g_enabled = true;
        copy_text(g_target, sizeof(g_target), serial);
        bambu_status_reset(&g_snapshot.printer);
        pthread_mutex_unlock(&g_lock);
        publish_connection(generation, BAMBU_MONITOR_AUTH_ERROR, false,
                           "登录信息不完整，请退出后重新登录");
        return;
    }

    args->generation = atomic_fetch_add(&g_generation, 1) + 1;
    pthread_mutex_lock(&g_lock);
    g_enabled = true;
    copy_text(g_target, sizeof(g_target), serial);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_CONNECTING;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "正在连接拓竹实时状态…");
    bambu_status_reset(&g_snapshot.printer);
    pthread_mutex_unlock(&g_lock);

    pthread_t th;
    if (pthread_create(&th, NULL, monitor_worker, args) == 0) {
        pthread_detach(th);
    } else {
        publish_connection(args->generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                           "无法启动实时状态任务");
        wipe(args, sizeof(*args));
        free(args);
    }
}

void bambu_monitor_stop(void)
{
    bambu_monitor_init();
    pthread_mutex_lock(&g_lock);
    bool already_stopped = !g_enabled && g_snapshot.state == BAMBU_MONITOR_STOPPED;
    pthread_mutex_unlock(&g_lock);
    if (already_stopped) return;
    atomic_fetch_add(&g_generation, 1);
    pthread_mutex_lock(&g_lock);
    g_enabled = false;
    g_target[0] = 0;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
    pthread_mutex_unlock(&g_lock);
}

void bambu_monitor_snapshot(bambu_monitor_snapshot_t *out)
{
    if (!out) return;
    bambu_monitor_init();
    pthread_mutex_lock(&g_lock);
    *out = g_snapshot;
    pthread_mutex_unlock(&g_lock);
}
