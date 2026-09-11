#define WIN32_LEAN_AND_MEAN
#include "bambu_monitor.h"
#include "bambu_cloud_internal.h"

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MQTT_PORT          "8883"
#define MQTT_TOKEN_MAX       2048
#define MQTT_PACKET_MAX  (64 * 1024)

typedef struct {
    LONG generation;
    bambu_cloud_region_t region;
    char serial[40];
    char user_id[96];
    char token[MQTT_TOKEN_MAX];
} monitor_args_t;

typedef struct {
    SOCKET socket;
    SSL_CTX *ctx;
    SSL *ssl;
} tls_session_t;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static bambu_monitor_snapshot_t g_snapshot;
static volatile LONG g_generation = 1;
static bool g_enabled;
static char g_target[40];

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static BOOL CALLBACK initialize_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    OPENSSL_init_ssl(0, NULL);
    return TRUE;
}

void bambu_monitor_init(void)
{
    InitOnceExecuteOnce(&g_once, initialize_once, NULL, NULL);
}

static bool generation_alive(LONG generation)
{
    return InterlockedCompareExchange(&g_generation, 0, 0) == generation;
}

static void publish_connection(LONG generation, bambu_monitor_state_t state,
                               bool connected, const char *message)
{
    if (!generation_alive(generation)) return;
    EnterCriticalSection(&g_lock);
    g_snapshot.state = state;
    g_snapshot.connected = connected;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    LeaveCriticalSection(&g_lock);
}

static bool load_windows_roots(SSL_CTX *ctx)
{
    X509_STORE *target = SSL_CTX_get_cert_store(ctx);
    HCERTSTORE roots = CertOpenSystemStoreA(0, "ROOT");
    if (!target || !roots) return false;
    int added = 0;
    PCCERT_CONTEXT cert = NULL;
    while ((cert = CertEnumCertificatesInStore(roots, cert)) != NULL) {
        const unsigned char *p = cert->pbCertEncoded;
        X509 *x = d2i_X509(NULL, &p, (long)cert->cbCertEncoded);
        if (!x) continue;
        if (X509_STORE_add_cert(target, x) == 1) added++;
        else ERR_clear_error(); /* duplicate roots are harmless */
        X509_free(x);
    }
    CertCloseStore(roots, 0);
    return added > 0;
}

static SOCKET tcp_connect(const char *host)
{
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, MQTT_PORT, &hints, &result) != 0) return INVALID_SOCKET;
    SOCKET sock = INVALID_SOCKET;
    for (struct addrinfo *it = result; it; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (sock != INVALID_SOCKET) {
        DWORD timeout = 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    }
    return sock;
}

static bool tls_open(tls_session_t *session, const char *host)
{
    memset(session, 0, sizeof(*session));
    session->socket = INVALID_SOCKET;
    session->ctx = SSL_CTX_new(TLS_client_method());
    if (!session->ctx) return false;
    SSL_CTX_set_min_proto_version(session->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(session->ctx, SSL_VERIFY_PEER, NULL);
    if (!load_windows_roots(session->ctx)) return false;
    session->socket = tcp_connect(host);
    if (session->socket == INVALID_SOCKET) return false;
    session->ssl = SSL_new(session->ctx);
    if (!session->ssl) return false;
    if (SSL_set_tlsext_host_name(session->ssl, host) != 1 ||
        SSL_set1_host(session->ssl, host) != 1 ||
        SSL_set_fd(session->ssl, (int)session->socket) != 1)
        return false;
    return SSL_connect(session->ssl) == 1;
}

static void tls_close(tls_session_t *session)
{
    if (session->ssl) {
        SSL_shutdown(session->ssl);
        SSL_free(session->ssl);
    }
    if (session->socket != INVALID_SOCKET) closesocket(session->socket);
    if (session->ctx) SSL_CTX_free(session->ctx);
    memset(session, 0, sizeof(*session));
    session->socket = INVALID_SOCKET;
}

static int ssl_write_all(SSL *ssl, const unsigned char *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int n = SSL_write(ssl, data + done, (int)(len - done));
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* 1=byte read, 0=one-second receive timeout, -1=connection closed/error. */
static int ssl_read_byte(SSL *ssl, unsigned char *out)
{
    int n = SSL_read(ssl, out, 1);
    if (n == 1) return 1;
    int e = SSL_get_error(ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
    if (e == SSL_ERROR_SYSCALL) {
        int wsa = WSAGetLastError();
        if (wsa == WSAETIMEDOUT || wsa == WSAEWOULDBLOCK) return 0;
    }
    return -1;
}

static int ssl_read_exact(SSL *ssl, unsigned char *out, size_t len, LONG generation)
{
    size_t done = 0;
    int idle = 0;
    while (done < len && generation_alive(generation)) {
        int n = SSL_read(ssl, out + done, (int)(len - done));
        if (n > 0) { done += (size_t)n; idle = 0; continue; }
        int e = SSL_get_error(ssl, n);
        int wsa = e == SSL_ERROR_SYSCALL ? WSAGetLastError() : 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE ||
            wsa == WSAETIMEDOUT || wsa == WSAEWOULDBLOCK) {
            if (++idle < 8) continue;
        }
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

static int mqtt_send(SSL *ssl, unsigned char type,
                     const unsigned char *body, size_t body_len)
{
    unsigned char head[5];
    head[0] = type;
    size_t n = encode_remaining(head + 1, body_len);
    if (ssl_write_all(ssl, head, n + 1) != 0) return -1;
    return body_len ? ssl_write_all(ssl, body, body_len) : 0;
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

static int mqtt_connect_packet(SSL *ssl, const char *client_id,
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
    int rc = ok ? mqtt_send(ssl, 0x10, body, used) : -1;
    SecureZeroMemory(body, cap);
    free(body);
    return rc;
}

/* 1=packet, 0=idle timeout, -1=disconnect/protocol error. */
static int mqtt_read_packet(SSL *ssl, unsigned char *type,
                            unsigned char **body, size_t *body_len,
                            LONG generation)
{
    unsigned char fixed;
    int first = ssl_read_byte(ssl, &fixed);
    if (first <= 0) return first;
    size_t remaining = 0, multiplier = 1;
    for (int i = 0; i < 4; i++) {
        unsigned char b;
        if (ssl_read_exact(ssl, &b, 1, generation) != 0) return -1;
        remaining += (size_t)(b & 0x7f) * multiplier;
        if (!(b & 0x80)) break;
        multiplier *= 128;
        if (i == 3) return -1;
    }
    if (remaining > MQTT_PACKET_MAX) return -1;
    unsigned char *payload = malloc(remaining + 1);
    if (!payload) return -1;
    if (remaining && ssl_read_exact(ssl, payload, remaining, generation) != 0) {
        free(payload);
        return -1;
    }
    payload[remaining] = 0;
    *type = fixed;
    *body = payload;
    *body_len = remaining;
    return 1;
}

static int wait_connack(SSL *ssl, LONG generation)
{
    for (int tries = 0; tries < 10 && generation_alive(generation); tries++) {
        unsigned char type, *body = NULL;
        size_t len = 0;
        int rc = mqtt_read_packet(ssl, &type, &body, &len, generation);
        if (rc == 0) continue;
        if (rc < 0) return -1;
        int result = ((type >> 4) == 2 && len >= 2) ? body[1] : -2;
        free(body);
        if (result != -2) return result;
    }
    return -1;
}

static int mqtt_subscribe(SSL *ssl, const char *topic)
{
    unsigned char body[160];
    size_t used = 0;
    body[used++] = 0;
    body[used++] = 1;
    if (!append_utf8(body, sizeof(body), &used, topic)) return -1;
    body[used++] = 0; /* QoS 0 */
    return mqtt_send(ssl, 0x82, body, used);
}

static int mqtt_publish(SSL *ssl, const char *topic, const char *json)
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
    int rc = ok ? mqtt_send(ssl, 0x30, body, used) : -1;
    free(body);
    return rc;
}

static void merge_publish(LONG generation, unsigned char fixed,
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

    EnterCriticalSection(&g_lock);
    if (generation_alive(generation) &&
        bambu_status_apply_json(&g_snapshot.printer,
                                (const char *)body + pos, len - pos)) {
        g_snapshot.connected = true;
        g_snapshot.state = BAMBU_MONITOR_CONNECTED;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), "实时状态已同步");
    }
    LeaveCriticalSection(&g_lock);
}

/* 0=normal disconnect, 4/5=MQTT auth failure, -1=network/protocol error. */
static int run_session(monitor_args_t *args)
{
    const char *host = args->region == BAMBU_CLOUD_REGION_CHINA
                     ? "cn.mqtt.bambulab.com" : "us.mqtt.bambulab.com";
    tls_session_t tls;
    if (!tls_open(&tls, host)) { tls_close(&tls); return -1; }

    unsigned char random[6] = {0};
    RAND_bytes(random, sizeof(random));
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "bblp_%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3], random[4], random[5]);
    if (mqtt_connect_packet(tls.ssl, client_id, args->user_id, args->token) != 0) {
        tls_close(&tls); return -1;
    }
    int ack = wait_connack(tls.ssl, args->generation);
    if (ack != 0) { tls_close(&tls); return ack; }

    char report[96], request[96];
    snprintf(report, sizeof(report), "device/%s/report", args->serial);
    snprintf(request, sizeof(request), "device/%s/request", args->serial);
    if (mqtt_subscribe(tls.ssl, report) != 0 ||
        mqtt_publish(tls.ssl, request,
            "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}") != 0) {
        tls_close(&tls); return -1;
    }
    publish_connection(args->generation, BAMBU_MONITOR_CONNECTED, true,
                       "已连接，正在读取打印机状态…");

    ULONGLONG last_tx = GetTickCount64();
    while (generation_alive(args->generation)) {
        unsigned char type, *body = NULL;
        size_t len = 0;
        int rc = mqtt_read_packet(tls.ssl, &type, &body, &len, args->generation);
        if (rc < 0) break;
        if (rc > 0) {
            if ((type >> 4) == 3) merge_publish(args->generation, type, body, len, report);
            free(body);
        }
        ULONGLONG now = GetTickCount64();
        if (now - last_tx >= 30000) {
            if (mqtt_send(tls.ssl, 0xC0, NULL, 0) != 0) break;
            last_tx = now;
        }
    }
    tls_close(&tls);
    return 0;
}

static DWORD WINAPI monitor_worker(LPVOID context)
{
    monitor_args_t *args = context;
    if (!args->user_id[0] &&
        !bambu_cloud_resolve_mqtt_user_id(args->token, args->region,
                                          args->user_id, sizeof(args->user_id))) {
        publish_connection(args->generation, BAMBU_MONITOR_AUTH_ERROR, false,
                           "无法取得云端实时监视身份，请重新登录");
        SecureZeroMemory(args->token, sizeof(args->token));
        free(args);
        return 0;
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
        DWORD waited = 0;
        while (waited < backoff_ms && generation_alive(args->generation)) {
            Sleep(100);
            waited += 100;
        }
        if (backoff_ms < 15000) backoff_ms *= 2;
        if (backoff_ms > 15000) backoff_ms = 15000;
    }
    SecureZeroMemory(args->token, sizeof(args->token));
    SecureZeroMemory(args->user_id, sizeof(args->user_id));
    free(args);
    return 0;
}

void bambu_monitor_start(const char *serial)
{
    bambu_monitor_init();
    if (!serial || !serial[0]) { bambu_monitor_stop(); return; }
    EnterCriticalSection(&g_lock);
    bool same = g_enabled && strcmp(g_target, serial) == 0;
    LeaveCriticalSection(&g_lock);
    if (same) return;

    monitor_args_t *args = calloc(1, sizeof(*args));
    if (!args) return;
    copy_text(args->serial, sizeof(args->serial), serial);
    if (!bambu_cloud_copy_mqtt_credentials(args->user_id, sizeof(args->user_id),
                                            args->token, sizeof(args->token),
                                            &args->region)) {
        SecureZeroMemory(args, sizeof(*args));
        free(args);
        LONG generation = InterlockedIncrement(&g_generation);
        EnterCriticalSection(&g_lock);
        g_enabled = true;
        copy_text(g_target, sizeof(g_target), serial);
        bambu_status_reset(&g_snapshot.printer);
        LeaveCriticalSection(&g_lock);
        publish_connection(generation, BAMBU_MONITOR_AUTH_ERROR, false,
                           "登录信息不完整，请退出后重新登录");
        return;
    }

    args->generation = InterlockedIncrement(&g_generation);
    EnterCriticalSection(&g_lock);
    g_enabled = true;
    copy_text(g_target, sizeof(g_target), serial);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_CONNECTING;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "正在连接拓竹实时状态…");
    bambu_status_reset(&g_snapshot.printer);
    LeaveCriticalSection(&g_lock);

    HANDLE thread = CreateThread(NULL, 0, monitor_worker, args, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
        publish_connection(args->generation, BAMBU_MONITOR_NETWORK_ERROR, false,
                           "无法启动实时状态任务");
        SecureZeroMemory(args, sizeof(*args));
        free(args);
    }
}

void bambu_monitor_stop(void)
{
    bambu_monitor_init();
    EnterCriticalSection(&g_lock);
    bool already_stopped = !g_enabled && g_snapshot.state == BAMBU_MONITOR_STOPPED;
    LeaveCriticalSection(&g_lock);
    if (already_stopped) return;
    InterlockedIncrement(&g_generation);
    EnterCriticalSection(&g_lock);
    g_enabled = false;
    g_target[0] = 0;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_MONITOR_STOPPED;
    bambu_status_reset(&g_snapshot.printer);
    LeaveCriticalSection(&g_lock);
}

void bambu_monitor_snapshot(bambu_monitor_snapshot_t *out)
{
    if (!out) return;
    bambu_monitor_init();
    EnterCriticalSection(&g_lock);
    *out = g_snapshot;
    LeaveCriticalSection(&g_lock);
}
