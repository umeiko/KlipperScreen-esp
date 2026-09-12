/*
 * Native POSIX Moonraker WebSocket transport（macOS / Linux 桌面端）。
 *
 * 协议契约与 moonraker_client_winhttp.c 完全一致，逐段对照移植：
 *   HTTP Upgrade 握手（Sec-WebSocket-Key/Accept 的 SHA1+base64 自实现，免依赖）
 *   → identify → server.info(等 klippy_connected) → objects.list → objects.subscribe
 *   → notify_status_update 增量 / RPC id 应答路由；
 * WS 帧编解码（客户端掩码、ping/pong、close、text+continuation 分片拼装）、
 * 阻塞 socket + 1s 接收超时驱动心跳 / klippy 重试 / 僵尸检测节拍，
 * 断线指数退避重连（1s 起、封顶 30s）。模型更新经 LVGL 上下文投递。
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1   /* macOS：保留 SO_NOSIGPIPE 等 BSD 扩展 */

#include "moonraker_client.h"
#include "printer_model_internal.h"
#include "app_settings.h"
#include "version.h"
#include "bsp.h"

#include "cJSON.h"
#include "lvgl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PENDING_MAX       16
#define RX_CHUNK_SIZE     8192
#define RX_MESSAGE_MAX    (256 * 1024)
#define RECONNECT_MAX_S   30
#define KLIPPY_RETRY_MS   5000
#define HEARTBEAT_MS      5000
#define ZOMBIE_MS         20000
#define CONNECT_TIMEOUT_MS 5000
#define HANDSHAKE_TIMEOUT_MS 5000
#define SEND_TIMEOUT_MS   5000
#define HTTP_HEADER_MAX   16384

/* RFC 6455 握手魔术串 */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* WS opcode */
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

typedef struct {
    int id;
    void (*cb)(cJSON *result);
    void (*cb_json)(char *result_json, void *ud);
    void *ud;
} pending_t;

typedef struct {
    void (*cb)(char *result_json, void *ud);
    void *ud;
    char *json;
} rpc_delivery_t;

/* 一条物理连接：fd + 握手时多收的字节（留给帧解析先消费） */
typedef struct {
    int fd;
    unsigned char carry[RX_CHUNK_SIZE];
    size_t carry_len;
} ws_conn_t;

typedef struct {
    bool fin;
    uint8_t opcode;
    unsigned char *payload;   /* malloc，调用方负责 free */
    uint64_t len;
} ws_frame_t;

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
/* 帧级发送串行化（WinHTTP 内部替我们做的事）：UI 线程的 RPC 与接收线程的
 * pong/close 回包可能并发写同一 socket，不串行化帧字节会交错。 */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_fd = -1;
static pending_t pending[PENDING_MAX];
static int next_id = 1;
static volatile int state_value = MOONRAKER_OFFLINE;
static volatile int worker_started;
static volatile int disabled;          /* moonraker_stop() 的 desired 态：worker 见此停机休眠 */
static volatile long reload_epoch = 1;
static volatile int reconnect_requested;
static uint64_t klippy_retry_due;
static uint64_t heartbeat_sent;
static uint64_t last_rx;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void set_state(moonraker_state_t state)
{
    state_value = (int)state;
}

/* ---------- LVGL delivery ---------- */
static void apply_in_lvgl(void *p)
{
    printer_model_apply_status_json((char *)p);
}

static void set_online_in_lvgl(void *p)
{
    printer_model_set_online((int)(intptr_t)p);
}

static void report_gcode_in_lvgl(void *p)
{
    printer_model_report_gcode_response((char *)p);
}

static void report_rpc_err_in_lvgl(void *p)
{
    printer_model_report_rpc_error((char *)p);
}

static void report_rtt_in_lvgl(void *p)
{
    printer_model_set_rtt((int)(intptr_t)p);
}

static void deliver_in_lvgl(void *p)
{
    rpc_delivery_t *d = p;
    d->cb(d->json, d->ud);
    free(d);
}

static void post_to_lvgl(lv_async_cb_t cb, void *p)
{
    bsp_lvgl_lock();
    lv_async_call(cb, p);
    bsp_lvgl_unlock();
}

static char *heap_copy(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void post_status(cJSON *status)
{
    if (!status) return;
    char *json = cJSON_PrintUnformatted(status);
    if (!json) return;
    char *copy = heap_copy(json);
    cJSON_free(json);
    if (copy) post_to_lvgl(apply_in_lvgl, copy);
}

static void fail_pending(void)
{
    pending_t failed[PENDING_MAX];
    int count = 0;

    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < PENDING_MAX; i++) {
        if (pending[i].id) {
            failed[count++] = pending[i];
            memset(&pending[i], 0, sizeof(pending[i]));
        }
    }
    pthread_mutex_unlock(&client_lock);

    for (int i = 0; i < count; i++) {
        if (!failed[i].cb_json) continue;
        rpc_delivery_t *d = malloc(sizeof(*d));
        if (!d) continue;
        d->cb = failed[i].cb_json;
        d->ud = failed[i].ud;
        d->json = NULL;
        post_to_lvgl(deliver_in_lvgl, d);
    }
}

static void mark_offline(void)
{
    set_state(MOONRAKER_OFFLINE);
    fail_pending();
    post_to_lvgl(set_online_in_lvgl, NULL);
}

/* ---------- RPC send ---------- */
static int alloc_pending_locked(void (*cb)(cJSON *),
                                void (*cb_json)(char *, void *), void *ud)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        if (pending[i].id == 0) {
            int id = next_id++;
            if (next_id <= 0) next_id = 1;
            pending[i].id = id;
            pending[i].cb = cb;
            pending[i].cb_json = cb_json;
            pending[i].ud = ud;
            return id;
        }
    }
    return 0;
}

static void free_pending_locked(int id)
{
    for (int i = 0; i < PENDING_MAX; i++) {
        if (pending[i].id == id) {
            memset(&pending[i], 0, sizeof(pending[i]));
            return;
        }
    }
}

static void rand_bytes(unsigned char *out, size_t n);   /* 定义在下方小工具区 */

static bool send_all(int fd, const void *vbuf, size_t len)
{
    const unsigned char *buf = vbuf;
    size_t sent = 0;
    uint64_t deadline = now_ms() + SEND_TIMEOUT_MS;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, buf + sent, len - sent, 0);  /* macOS 靠 SO_NOSIGPIPE */
#endif
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (now_ms() >= deadline) return false;
            continue;   /* SO_SNDTIMEO 到点 / 暂时写不动，限时内继续试 */
        }
        return false;
    }
    return true;
}

/* 客户端帧必须带掩码（RFC 6455 §5.3） */
static bool ws_send_frame(int fd, uint8_t opcode, const void *payload, size_t len)
{
    unsigned char hdr[14];
    size_t hlen = 0;
    hdr[hlen++] = 0x80 | opcode;
    if (len < 126) {
        hdr[hlen++] = 0x80 | (unsigned char)len;
    } else if (len <= 0xFFFF) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (unsigned char)(len >> 8);
        hdr[hlen++] = (unsigned char)len;
    } else {
        hdr[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            hdr[hlen++] = (unsigned char)((uint64_t)len >> (i * 8));
    }
    unsigned char mask[4];
    rand_bytes(mask, sizeof(mask));
    memcpy(hdr + hlen, mask, sizeof(mask));
    hlen += sizeof(mask);

    bool ok;
    pthread_mutex_lock(&send_lock);
    ok = send_all(fd, hdr, hlen);
    if (ok && len) {
        unsigned char *masked = malloc(len);
        if (!masked) {
            ok = false;
        } else {
            const unsigned char *src = payload;
            for (size_t i = 0; i < len; i++) masked[i] = src[i] ^ mask[i & 3];
            ok = send_all(fd, masked, len);
            free(masked);
        }
    }
    pthread_mutex_unlock(&send_lock);
    return ok;
}

static bool ws_send_text(int fd, const char *text, size_t len)
{
    return ws_send_frame(fd, WS_OP_TEXT, text, len);
}

static bool send_rpc_full(const char *method, const char *params_json,
                          void (*cb)(cJSON *),
                          void (*cb_json)(char *, void *), void *ud)
{
    if (!method || !method[0]) return false;
    if (disabled) return false;   /* stop 后抑制所有 RPC */

    size_t cap = strlen(method) + (params_json ? strlen(params_json) : 0) + 96;
    char *frame = malloc(cap);
    if (!frame) return false;

    bool ok = false;
    pthread_mutex_lock(&client_lock);
    int fd = active_fd;
    int id = fd >= 0 ? alloc_pending_locked(cb, cb_json, ud) : 0;
    pthread_mutex_unlock(&client_lock);
    /* 发送不持 client_lock（阻塞上限 SEND_TIMEOUT_MS）；与 remove_ws 竞态由
     * close_connection 的 shutdown 兜底：在飞的 send 会立即失败走重连。 */
    if (id) {
        int n = params_json
            ? snprintf(frame, cap,
                       "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                       id, method, params_json)
            : snprintf(frame, cap,
                       "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                       id, method);
        if (n > 0 && (size_t)n < cap)
            ok = ws_send_text(fd, frame, (size_t)n);
        if (!ok) {
            pthread_mutex_lock(&client_lock);
            free_pending_locked(id);
            pthread_mutex_unlock(&client_lock);
            reconnect_requested = 1;
        }
    }
    free(frame);
    return ok;
}

static bool send_rpc_cb(const char *method, const char *params_json,
                        void (*cb)(cJSON *))
{
    return send_rpc_full(method, params_json, cb, NULL, NULL);
}

bool moonraker_send_rpc(const char *method, const char *params_json)
{
    return send_rpc_full(method, params_json, NULL, NULL, NULL);
}

bool moonraker_rpc(const char *method, const char *params_json,
                   void (*cb)(char *, void *), void *ud)
{
    if (!cb) return false;
    return send_rpc_full(method, params_json, NULL, cb, ud);
}

/* ---------- Moonraker protocol ---------- */
static void handshake_step_subscribe(void);

static void on_subscribe_result(cJSON *result)
{
    post_status(cJSON_GetObjectItem(result, "status"));
    set_state(MOONRAKER_READY);
    post_to_lvgl(set_online_in_lvgl, (void *)(intptr_t)1);
}

static void handshake_step_subscribe(void)
{
    const char *params =
        "{\"objects\":{"
        "\"webhooks\":null,"
        "\"print_stats\":[\"state\",\"filename\",\"print_duration\",\"total_duration\",\"message\"],"
        "\"virtual_sdcard\":[\"progress\",\"is_active\"],"
        "\"display_status\":[\"progress\",\"message\"],"
        "\"gcode_move\":[\"speed_factor\",\"extrude_factor\"],"
        "\"toolhead\":[\"position\",\"homed_axes\"],"
        "\"extruder\":[\"temperature\",\"target\",\"power\"],"
        "\"heater_bed\":[\"temperature\",\"target\",\"power\"],"
        "\"fan\":[\"speed\"],"
        "\"idle_timeout\":[\"state\"],"
        "\"pause_resume\":[\"is_paused\"]"
        "}}";
    send_rpc_cb("printer.objects.subscribe", params, on_subscribe_result);
}

static void on_objects_list_result(cJSON *result)
{
    (void)result;
    handshake_step_subscribe();
}

static void on_server_info(cJSON *result)
{
    cJSON *connected = cJSON_GetObjectItem(result, "klippy_connected");
    if (cJSON_IsBool(connected) && cJSON_IsTrue(connected)) {
        klippy_retry_due = 0;
        send_rpc_cb("printer.objects.list", NULL, on_objects_list_result);
    } else {
        klippy_retry_due = now_ms() + KLIPPY_RETRY_MS;
    }
}

static void on_heartbeat_result(cJSON *result)
{
    (void)result;
    uint64_t sent = heartbeat_sent;
    int ms = sent ? (int)(now_ms() - sent) : 0;
    post_to_lvgl(report_rtt_in_lvgl, (void *)(intptr_t)ms);
}

static void handshake_begin(void)
{
#ifdef __APPLE__
    const char *client_name = "klipper-remote-macos";
#else
    const char *client_name = "klipper-remote-linux";
#endif
    char params[256];
    snprintf(params, sizeof(params),
             "{\"client_name\":\"%s\","
             "\"version\":\"" KR_VERSION "\",\"type\":\"display\","
             "\"url\":\"https://github.com/umeiko/KlipperScreen-esp\"}",
             client_name);
    send_rpc_cb("server.connection.identify", params, NULL);
    send_rpc_cb("server.info", NULL, on_server_info);
}

static void handle_notify(const char *method, cJSON *params)
{
    if (strcmp(method, "notify_status_update") == 0) {
        post_status(cJSON_GetArrayItem(params, 0));
    } else if (strcmp(method, "notify_gcode_response") == 0) {
        cJSON *s = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsString(s) && s->valuestring &&
            strncmp(s->valuestring, "!!", 2) == 0) {
            char *copy = heap_copy(s->valuestring);
            if (copy) post_to_lvgl(report_gcode_in_lvgl, copy);
        }
    } else if (strcmp(method, "notify_klippy_ready") == 0) {
        handshake_step_subscribe();
    } else if (strcmp(method, "notify_klippy_shutdown") == 0 ||
               strcmp(method, "notify_klippy_disconnected") == 0) {
        const char *json = strcmp(method, "notify_klippy_shutdown") == 0
                         ? "{\"webhooks\":{\"state\":\"shutdown\"}}"
                         : "{\"webhooks\":{\"state\":\"disconnected\"}}";
        char *copy = heap_copy(json);
        if (copy) post_to_lvgl(apply_in_lvgl, copy);
    }
}

static void on_ws_message(const char *data, size_t len)
{
    cJSON *msg = cJSON_ParseWithLength(data, len);
    if (!msg) return;

    cJSON *id_obj = cJSON_GetObjectItem(msg, "id");
    if (cJSON_IsNumber(id_obj)) {
        pending_t found = {0};
        int id = (int)id_obj->valuedouble;
        pthread_mutex_lock(&client_lock);
        for (int i = 0; i < PENDING_MAX; i++) {
            if (pending[i].id == id) {
                found = pending[i];
                memset(&pending[i], 0, sizeof(pending[i]));
                break;
            }
        }
        pthread_mutex_unlock(&client_lock);

        if (found.id) {
            cJSON *error = cJSON_GetObjectItem(msg, "error");
            if (error) {
                cJSON *message = cJSON_GetObjectItem(error, "message");
                const char *text = cJSON_IsString(message) ? message->valuestring : "RPC error";
                if (!found.cb && !found.cb_json) {
                    char *copy = heap_copy(text);
                    if (copy) post_to_lvgl(report_rpc_err_in_lvgl, copy);
                }
                if (found.cb_json) {
                    rpc_delivery_t *d = malloc(sizeof(*d));
                    if (d) {
                        d->cb = found.cb_json; d->ud = found.ud; d->json = NULL;
                        post_to_lvgl(deliver_in_lvgl, d);
                    }
                }
            } else if (found.cb) {
                found.cb(cJSON_GetObjectItem(msg, "result"));
            } else if (found.cb_json) {
                cJSON *result = cJSON_GetObjectItem(msg, "result");
                char *json = result ? cJSON_PrintUnformatted(result) : NULL;
                rpc_delivery_t *d = malloc(sizeof(*d));
                if (d) {
                    d->cb = found.cb_json; d->ud = found.ud; d->json = json;
                    post_to_lvgl(deliver_in_lvgl, d);
                } else {
                    cJSON_free(json);
                }
            }
        }
    } else {
        cJSON *method = cJSON_GetObjectItem(msg, "method");
        if (cJSON_IsString(method))
            handle_notify(method->valuestring, cJSON_GetObjectItem(msg, "params"));
    }
    cJSON_Delete(msg);
}

/* ---------- 小工具：SHA1 / base64 / url 编码（握手用，自实现免依赖） ---------- */
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t h[5], const unsigned char *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/* RFC 3174 SHA1 */
static void sha1(const unsigned char *data, size_t len, unsigned char out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = len;
    while (len >= 64) { sha1_block(h, data); data += 64; len -= 64; }

    unsigned char tail[128];
    size_t t = len;
    memcpy(tail, data, len);
    tail[t++] = 0x80;
    if (t > 56) {
        while (t < 64) tail[t++] = 0;
        sha1_block(h, tail);
        t = 0;
    }
    while (t < 56) tail[t++] = 0;
    uint64_t bits = (uint64_t)total * 8;
    for (int i = 7; i >= 0; i--) tail[t++] = (unsigned char)(bits >> (i * 8));
    sha1_block(h, tail);

    for (int i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(h[i] >> 24);
        out[i*4+1] = (unsigned char)(h[i] >> 16);
        out[i*4+2] = (unsigned char)(h[i] >> 8);
        out[i*4+3] = (unsigned char)h[i];
    }
}

static size_t base64_encode(const unsigned char *in, size_t len, char *out, size_t out_sz)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4 + 1;
    if (out_sz < need) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        size_t rem = len - i;
        if (rem > 1) v |= (unsigned)in[i+1] << 8;
        if (rem > 2) v |= in[i+2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = rem > 1 ? b64[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* 握手随机数：/dev/urandom，兜底弱随机（仅协议随机数，无安全语义） */
static void rand_bytes(unsigned char *out, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
    srand((unsigned)(now_ms() ^ (uint64_t)(uintptr_t)out));
    for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(rand() & 0xFF);
}

static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && out + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%'; dst[out++] = hex[c >> 4]; dst[out++] = hex[c & 15];
        }
    }
    dst[out] = 0;
}

/* 在 HTTP 头块里按行找头部（大小写不敏感），值去首尾空白拷进 out */
static bool get_header_value(const char *hdr, size_t len, const char *name,
                             char *out, size_t out_sz)
{
    size_t name_len = strlen(name);
    size_t pos = 0;
    while (pos < len) {
        const char *line = hdr + pos;
        const char *eol = memchr(line, '\n', len - pos);
        size_t line_len = eol ? (size_t)(eol - line) : len - pos;
        if (line_len > name_len + 1 &&
            strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            const char *vend = line + line_len;
            while (v < vend && (*v == ' ' || *v == '\t')) v++;
            while (vend > v && (vend[-1] == '\r' || vend[-1] == ' ' || vend[-1] == '\t')) vend--;
            size_t vn = (size_t)(vend - v);
            if (vn >= out_sz) vn = out_sz - 1;
            memcpy(out, v, vn);
            out[vn] = 0;
            return true;
        }
        if (!eol) break;
        pos += line_len + 1;
    }
    return false;
}

/* ---------- POSIX socket + WebSocket 传输 ---------- */
static int tcp_connect(const char *host, uint16_t port, int timeout_ms)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        /* 非阻塞 connect + poll 实现连接超时（对齐 winhttp 的 5s connect timeout） */
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { s, POLLOUT, 0 };
            rc = poll(&pfd, 1, timeout_ms);
            if (rc > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                rc = (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
                     ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            fcntl(s, F_SETFL, flags);   /* 回到阻塞模式 */
            fd = s;
        } else {
            close(s);
        }
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    /* macOS 无 MSG_NOSIGNAL：关掉 SIGPIPE，写断管返回 EPIPE 而不是杀进程 */
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    /* 接收超时 1s：驱动心跳 / klippy 重试 / 僵尸检测节拍
     *（对齐 winhttp 的 1000ms receive timeout） */
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static bool ws_handshake(ws_conn_t *conn, const moonraker_conf_t *conf)
{
    unsigned char nonce[16];
    rand_bytes(nonce, sizeof(nonce));
    char key[32];
    base64_encode(nonce, sizeof(nonce), key, sizeof(key));

    char path[512] = "/websocket";
    if (conf->api_key[0]) {
        char token[256];
        url_encode(conf->api_key, token, sizeof(token));
        snprintf(path, sizeof(path), "/websocket?token=%s", token);
    }

    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, conf->host, (unsigned)(conf->port ? conf->port : 7125), key);
    if (n <= 0 || (size_t)n >= sizeof(req)) return false;
    if (!send_all(conn->fd, req, (size_t)n)) return false;

    /* 读响应头到 \r\n\r\n（至多 HTTP_HEADER_MAX 字节）；多收的字节留进 carry */
    unsigned char hdr[HTTP_HEADER_MAX];
    size_t hlen = 0;
    int header_end = -1;
    uint64_t deadline = now_ms() + HANDSHAKE_TIMEOUT_MS;
    while (hlen < sizeof(hdr) && now_ms() < deadline) {
        ssize_t r = recv(conn->fd, hdr + hlen, sizeof(hdr) - hlen, 0);
        if (r > 0) {
            hlen += (size_t)r;
            for (size_t i = 0; i + 3 < hlen; i++) {
                if (hdr[i] == '\r' && hdr[i+1] == '\n' &&
                    hdr[i+2] == '\r' && hdr[i+3] == '\n') {
                    header_end = (int)i;
                    break;
                }
            }
            if (header_end >= 0) break;
        } else if (r == 0) {
            return false;   /* 对端关断 */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
    }
    if (header_end < 0) return false;

    /* 状态行必须 101 */
    if (memcmp(hdr, "HTTP/", 5) != 0) return false;
    int status = 0;
    for (size_t i = 0; i < (size_t)header_end; i++) {
        if (hdr[i] == ' ') {
            status = atoi((const char *)hdr + i + 1);
            break;
        }
    }
    if (status != 101) return false;

    /* 校验 Sec-WebSocket-Accept = base64(SHA1(key + GUID))，防连到假服务器 */
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
    unsigned char digest[20];
    sha1((const unsigned char *)concat, strlen(concat), digest);
    char expect[32];
    base64_encode(digest, sizeof(digest), expect, sizeof(expect));

    char accept_val[64];
    if (!get_header_value((const char *)hdr, (size_t)header_end,
                          "Sec-WebSocket-Accept", accept_val, sizeof(accept_val)))
        return false;
    if (strcmp(accept_val, expect) != 0) return false;

    size_t extra = hlen - ((size_t)header_end + 4);
    if (extra) {
        if (extra > sizeof(conn->carry)) return false;   /* 保守断开 */
        memcpy(conn->carry, hdr + header_end + 4, extra);
        conn->carry_len = extra;
    }
    return true;
}

/* 读一批字节（先消费 carry）：>0 读到，0 超时（SO_RCVTIMEO 到点），-1 断开/错误 */
static int conn_read(ws_conn_t *conn, void *buf, size_t len)
{
    if (conn->carry_len) {
        size_t n = conn->carry_len < len ? conn->carry_len : len;
        memcpy(buf, conn->carry, n);
        memmove(conn->carry, conn->carry + n, conn->carry_len - n);
        conn->carry_len -= n;
        return (int)n;
    }
    ssize_t r = recv(conn->fd, buf, len, 0);
    if (r > 0) return (int)r;
    if (r == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

/* 填满 buf：1=成功，0=首字节前就超时，-1=断开/错误/外部中止（epoch 变更等） */
static int conn_read_exact(ws_conn_t *conn, void *vbuf, size_t len, long epoch)
{
    unsigned char *buf = vbuf;
    size_t got = 0;
    while (got < len) {
        if (reload_epoch != epoch || reconnect_requested) return -1;
        int r = conn_read(conn, buf + got, len - got);
        if (r < 0) return -1;
        if (r == 0) {
            if (got == 0) return 0;
            continue;   /* 帧已收一半：继续等，每秒醒一次查中止条件 */
        }
        got += (size_t)r;
    }
    return 1;
}

/* 收一个完整 WS 帧：1=成功，0=超时（无任何字节），-1=断开/协议错误 */
static int ws_recv_frame(ws_conn_t *conn, ws_frame_t *fr, long epoch)
{
    unsigned char hdr[2];
    int r = conn_read_exact(conn, hdr, 2, epoch);
    if (r <= 0) return r;

    fr->fin = (hdr[0] & 0x80) != 0;
    fr->opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    /* 扩展长度字段：126=后续 2 字节 BE，127=后续 8 字节 BE */
    if (len == 126) {
        unsigned char ext[2];
        if (conn_read_exact(conn, ext, 2, epoch) <= 0) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (conn_read_exact(conn, ext, 8, epoch) <= 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > RX_MESSAGE_MAX) return -1;   /* 超上限直接判死，走重连 */

    unsigned char mask[4] = {0};
    if (masked && conn_read_exact(conn, mask, 4, epoch) <= 0) return -1;

    fr->payload = malloc(len ? (size_t)len : 1);
    if (!fr->payload) return -1;
    fr->len = len;
    if (len && conn_read_exact(conn, fr->payload, (size_t)len, epoch) <= 0) {
        free(fr->payload);
        fr->payload = NULL;
        return -1;
    }
    /* 服务端按理不掩码，收到了也照解 */
    if (masked)
        for (uint64_t i = 0; i < len; i++) fr->payload[i] ^= mask[i & 3];
    return 1;
}

static bool connect_websocket(const moonraker_conf_t *conf, ws_conn_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    int fd = tcp_connect(conf->host, conf->port ? conf->port : 7125,
                         CONNECT_TIMEOUT_MS);
    if (fd < 0) return false;
    out->fd = fd;
    if (!ws_handshake(out, conf)) {
        close(fd);
        out->fd = -1;
        return false;
    }
    return true;
}

static void close_connection(ws_conn_t *conn)
{
    if (conn->fd >= 0) {
        /* 先 shutdown 让可能在飞的 send（UI 线程）立即失败，再 close */
        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);
    }
    conn->fd = -1;
    conn->carry_len = 0;
}

static void install_ws(int fd)
{
    pthread_mutex_lock(&client_lock);
    memset(pending, 0, sizeof(pending));
    active_fd = fd;
    pthread_mutex_unlock(&client_lock);
}

static void remove_ws(int fd)
{
    pthread_mutex_lock(&client_lock);
    if (active_fd == fd) active_fd = -1;
    pthread_mutex_unlock(&client_lock);
}

static bool append_rx(char **message, size_t *len, size_t *cap,
                      const unsigned char *chunk, size_t chunk_len)
{
    if (*len + chunk_len + 1 > RX_MESSAGE_MAX) return false;
    if (*len + chunk_len + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : RX_CHUNK_SIZE * 2;
        while (next < *len + chunk_len + 1) next *= 2;
        char *grown = realloc(*message, next);
        if (!grown) return false;
        *message = grown;
        *cap = next;
    }
    memcpy(*message + *len, chunk, chunk_len);
    *len += chunk_len;
    (*message)[*len] = 0;
    return true;
}

static void receive_loop(ws_conn_t *conn, long epoch)
{
    char *message = NULL;
    size_t message_len = 0, message_cap = 0;
    bool connected = true;

    klippy_retry_due = 0;
    heartbeat_sent = 0;
    last_rx = now_ms();
    handshake_begin();

    while (connected && reload_epoch == epoch && !reconnect_requested) {
        ws_frame_t fr;
        memset(&fr, 0, sizeof(fr));
        int r = ws_recv_frame(conn, &fr, epoch);
        uint64_t now = now_ms();

        if (r < 0) {
            connected = false;
        } else if (r > 0) {
            last_rx = now;
            switch (fr.opcode) {
            case WS_OP_CLOSE:
                /* 回一个 close（控制帧 payload 至多 125 字节）后断开 */
                ws_send_frame(conn->fd, WS_OP_CLOSE, fr.payload,
                              fr.len > 125 ? 0 : (size_t)fr.len);
                connected = false;
                break;
            case WS_OP_PING:
                ws_send_frame(conn->fd, WS_OP_PONG, fr.payload, (size_t)fr.len);
                break;
            case WS_OP_PONG:
                break;   /* last_rx 已刷新 */
            case WS_OP_TEXT:
            case WS_OP_CONT:
                /* text 首片 + continuation 分片：拼到 FIN 再整条派发 */
                if (!append_rx(&message, &message_len, &message_cap,
                               fr.payload, (size_t)fr.len)) {
                    connected = false;
                } else if (fr.fin) {
                    on_ws_message(message, message_len);
                    message_len = 0;
                }
                break;
            default:
                break;   /* binary 等：忽略 */
            }
            free(fr.payload);
        }
        /* r == 0：接收超时，落到下面的节拍处理 */

        now = now_ms();
        if (klippy_retry_due && now >= klippy_retry_due) {
            klippy_retry_due = 0;
            send_rpc_cb("server.info", NULL, on_server_info);
        }
        if (moonraker_state() == MOONRAKER_READY &&
            (!heartbeat_sent || now - heartbeat_sent >= HEARTBEAT_MS)) {
            heartbeat_sent = now;
            send_rpc_cb("server.info", NULL, on_heartbeat_result);
        }
        if (moonraker_state() == MOONRAKER_READY && now - last_rx > ZOMBIE_MS)
            connected = false;
    }

    free(message);
}

static bool wait_backoff(int seconds, long epoch)
{
    for (int i = 0; i < seconds * 10; i++) {
        if (reload_epoch != epoch) return false;
        sleep_ms(100);
    }
    return true;
}

static void *worker_main(void *arg)
{
    (void)arg;
    int backoff = 1;

    for (;;) {
        long epoch = reload_epoch;
        if (disabled) {
            /* moonraker_stop()：worker 不退出，原地休眠等 start 唤醒（100ms 轮询） */
            set_state(MOONRAKER_OFFLINE);
            while (disabled) sleep_ms(100);
            continue;
        }
        moonraker_conf_t conf;
        if (!settings_load_moonraker(&conf)) {
            set_state(MOONRAKER_OFFLINE);
            wait_backoff(1, epoch);
            continue;
        }

        set_state(MOONRAKER_CONNECTING);
        fprintf(stderr, "Moonraker: connecting to %s:%u\n",
                conf.host, (unsigned)(conf.port ? conf.port : 7125));
        ws_conn_t conn;
        if (!connect_websocket(&conf, &conn)) {
            fprintf(stderr, "Moonraker: connection failed, retry in %ds\n", backoff);
            mark_offline();
            if (wait_backoff(backoff, epoch))
                backoff = backoff * 2 > RECONNECT_MAX_S ? RECONNECT_MAX_S : backoff * 2;
            else
                backoff = 1;
            continue;
        }

        fprintf(stderr, "Moonraker: WebSocket connected\n");
        backoff = 1;
        set_state(MOONRAKER_CONNECTING);
        install_ws(conn.fd);
        receive_loop(&conn, epoch);
        remove_ws(conn.fd);
        close_connection(&conn);
        mark_offline();
    }
    return NULL;
}

static void ensure_worker(void)
{
    pthread_mutex_lock(&client_lock);
    if (worker_started) {
        pthread_mutex_unlock(&client_lock);
        return;
    }
    worker_started = 1;
    pthread_mutex_unlock(&client_lock);

    pthread_t thread;
    if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
        pthread_detach(thread);
    } else {
        worker_started = 0;
        fprintf(stderr, "Moonraker: unable to start network worker\n");
    }
}

void moonraker_start(void)
{
    disabled = 0;   /* stop 之后靠 start 唤醒休眠的 worker */
    ensure_worker();
}

void moonraker_stop(void)
{
    if (disabled) return;   /* 幂等 */
    disabled = 1;
    /* 唤醒并逼退 worker：epoch 失配让 conn_read_exact（阻塞 recv ≤1s 超时）
     * 和 wait_backoff（100ms 切片）立即退出，socket 在 worker 线程里
     * shutdown+close，调用方（可能是 LVGL 线程）不做任何阻塞关闭。 */
    reload_epoch++;
    reconnect_requested = 1;
}

void moonraker_reload(void)
{
    reload_epoch++;
    reconnect_requested = 1;
    ensure_worker();
}

moonraker_state_t moonraker_state(void)
{
    return (moonraker_state_t)state_value;
}
