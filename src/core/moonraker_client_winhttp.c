/*
 * Native Windows Moonraker WebSocket transport.
 *
 * The protocol contract mirrors moonraker_client.c on ESP32, while WinHTTP
 * owns the socket and reconnect loop. Model updates are posted to the LVGL
 * thread through the desktop BSP lock.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "moonraker_client.h"
#include "printer_model_internal.h"
#include "app_settings.h"
#include "version.h"
#include "bsp.h"

#include "cJSON.h"
#include "lvgl.h"

#include <windows.h>
#include <winhttp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PENDING_MAX       16
#define RX_CHUNK_SIZE     8192
#define RX_MESSAGE_MAX    (256 * 1024)
#define RECONNECT_MAX_S   30
#define KLIPPY_RETRY_MS   5000
#define HEARTBEAT_MS      5000
#define ZOMBIE_MS         20000

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

typedef struct {
    HINTERNET session;
    HINTERNET connection;
    HINTERNET websocket;
} ws_connection_t;

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION client_lock;
static HINTERNET active_ws;
static pending_t pending[PENDING_MAX];
static int next_id = 1;
static volatile LONG state_value = MOONRAKER_OFFLINE;
static volatile LONG worker_started;
static volatile LONG reload_epoch = 1;
static volatile LONG reconnect_requested;
static ULONGLONG klippy_retry_due;
static ULONGLONG heartbeat_sent;
static ULONGLONG last_rx;

static BOOL CALLBACK init_client_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&client_lock);
    return TRUE;
}

static void ensure_init(void)
{
    InitOnceExecuteOnce(&init_once, init_client_once, NULL, NULL);
}

static void set_state(moonraker_state_t state)
{
    InterlockedExchange(&state_value, (LONG)state);
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

    ensure_init();
    EnterCriticalSection(&client_lock);
    for (int i = 0; i < PENDING_MAX; i++) {
        if (pending[i].id) {
            failed[count++] = pending[i];
            memset(&pending[i], 0, sizeof(pending[i]));
        }
    }
    LeaveCriticalSection(&client_lock);

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

static bool send_rpc_full(const char *method, const char *params_json,
                          void (*cb)(cJSON *),
                          void (*cb_json)(char *, void *), void *ud)
{
    if (!method || !method[0]) return false;
    ensure_init();

    size_t cap = strlen(method) + (params_json ? strlen(params_json) : 0) + 96;
    char *frame = malloc(cap);
    if (!frame) return false;

    bool ok = false;
    EnterCriticalSection(&client_lock);
    HINTERNET ws = active_ws;
    int id = ws ? alloc_pending_locked(cb, cb_json, ud) : 0;
    if (id) {
        int n = params_json
            ? snprintf(frame, cap,
                       "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                       id, method, params_json)
            : snprintf(frame, cap,
                       "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                       id, method);
        if (n > 0 && (size_t)n < cap) {
            DWORD err = WinHttpWebSocketSend(ws,
                WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, frame, (DWORD)n);
            ok = err == NO_ERROR;
        }
        if (!ok) {
            free_pending_locked(id);
            InterlockedExchange(&reconnect_requested, 1);
        }
    }
    LeaveCriticalSection(&client_lock);
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
        klippy_retry_due = GetTickCount64() + KLIPPY_RETRY_MS;
    }
}

static void on_heartbeat_result(cJSON *result)
{
    (void)result;
    ULONGLONG sent = heartbeat_sent;
    int ms = sent ? (int)(GetTickCount64() - sent) : 0;
    post_to_lvgl(report_rtt_in_lvgl, (void *)(intptr_t)ms);
}

static void handshake_begin(void)
{
    send_rpc_cb("server.connection.identify",
                "{\"client_name\":\"klipper-remote-windows\","
                "\"version\":\"" KR_VERSION "\",\"type\":\"display\","
                "\"url\":\"https://github.com/umeiko/KlipperScreen-esp\"}",
                NULL);
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
        EnterCriticalSection(&client_lock);
        for (int i = 0; i < PENDING_MAX; i++) {
            if (pending[i].id == id) {
                found = pending[i];
                memset(&pending[i], 0, sizeof(pending[i]));
                break;
            }
        }
        LeaveCriticalSection(&client_lock);

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

/* ---------- WinHTTP transport ---------- */
static bool to_wide(const char *src, wchar_t *dst, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, cap);
    if (!n) n = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, cap);
    return n > 0;
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

static bool connect_websocket(const moonraker_conf_t *conf, ws_connection_t *out)
{
    memset(out, 0, sizeof(*out));
    wchar_t host[128], path[512];
    char path_utf8[512] = "/websocket";
    if (conf->api_key[0]) {
        char token[256];
        url_encode(conf->api_key, token, sizeof(token));
        snprintf(path_utf8, sizeof(path_utf8), "/websocket?token=%s", token);
    }
    if (!to_wide(conf->host, host, (int)(sizeof(host) / sizeof(host[0]))) ||
        !to_wide(path_utf8, path, (int)(sizeof(path) / sizeof(path[0]))))
        return false;

    out->session = WinHttpOpen(L"Klipper Remote/" KR_VERSION,
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!out->session) return false;
    WinHttpSetTimeouts(out->session, 5000, 5000, 5000, 1000);

    out->connection = WinHttpConnect(out->session, host,
        conf->port ? conf->port : 7125, 0);
    HINTERNET request = NULL;
    if (out->connection) {
        request = WinHttpOpenRequest(out->connection, L"GET", path, NULL,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    }
    if (request &&
        WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL)) {
        out->websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    }
    if (request) WinHttpCloseHandle(request);
    return out->websocket != NULL;
}

static void close_connection(ws_connection_t *conn)
{
    if (conn->websocket) {
        WinHttpWebSocketClose(conn->websocket,
                              WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(conn->websocket);
    }
    if (conn->connection) WinHttpCloseHandle(conn->connection);
    if (conn->session) WinHttpCloseHandle(conn->session);
    memset(conn, 0, sizeof(*conn));
}

static void install_ws(HINTERNET ws)
{
    EnterCriticalSection(&client_lock);
    memset(pending, 0, sizeof(pending));
    active_ws = ws;
    LeaveCriticalSection(&client_lock);
}

static void remove_ws(HINTERNET ws)
{
    EnterCriticalSection(&client_lock);
    if (active_ws == ws) active_ws = NULL;
    LeaveCriticalSection(&client_lock);
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

static void receive_loop(HINTERNET ws, LONG epoch)
{
    unsigned char chunk[RX_CHUNK_SIZE];
    char *message = NULL;
    size_t message_len = 0, message_cap = 0;
    bool connected = true;

    klippy_retry_due = 0;
    heartbeat_sent = 0;
    last_rx = GetTickCount64();
    handshake_begin();

    while (connected && InterlockedCompareExchange(&reload_epoch, 0, 0) == epoch &&
           !InterlockedExchange(&reconnect_requested, 0)) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(ws, chunk, sizeof(chunk), &got, &type);
        ULONGLONG now = GetTickCount64();

        if (err == NO_ERROR) {
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                connected = false;
            } else if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                       type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                last_rx = now;
                if (!append_rx(&message, &message_len, &message_cap, chunk, got)) {
                    connected = false;
                } else if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                    on_ws_message(message, message_len);
                    message_len = 0;
                }
            }
        } else if (err != ERROR_WINHTTP_TIMEOUT) {
            connected = false;
        }

        now = GetTickCount64();
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

static bool wait_backoff(int seconds, LONG epoch)
{
    for (int i = 0; i < seconds * 10; i++) {
        if (InterlockedCompareExchange(&reload_epoch, 0, 0) != epoch) return false;
        Sleep(100);
    }
    return true;
}

static DWORD WINAPI worker_main(void *arg)
{
    (void)arg;
    int backoff = 1;

    for (;;) {
        LONG epoch = InterlockedCompareExchange(&reload_epoch, 0, 0);
        moonraker_conf_t conf;
        if (!settings_load_moonraker(&conf)) {
            set_state(MOONRAKER_OFFLINE);
            wait_backoff(1, epoch);
            continue;
        }

        set_state(MOONRAKER_CONNECTING);
        fprintf(stderr, "Moonraker: connecting to %s:%u\n",
                conf.host, (unsigned)(conf.port ? conf.port : 7125));
        ws_connection_t conn;
        if (!connect_websocket(&conf, &conn)) {
            DWORD error = GetLastError();
            close_connection(&conn);
            fprintf(stderr, "Moonraker: connection failed (%lu), retry in %ds\n",
                    (unsigned long)error, backoff);
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
        install_ws(conn.websocket);
        receive_loop(conn.websocket, epoch);
        remove_ws(conn.websocket);
        close_connection(&conn);
        mark_offline();
    }
}

static void ensure_worker(void)
{
    ensure_init();
    if (InterlockedCompareExchange(&worker_started, 1, 0) != 0) return;
    HANDLE thread = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&worker_started, 0);
        fprintf(stderr, "Moonraker: unable to start network worker (%lu)\n",
                (unsigned long)GetLastError());
    }
}

void moonraker_start(void)
{
    ensure_worker();
}

void moonraker_reload(void)
{
    InterlockedIncrement(&reload_epoch);
    InterlockedExchange(&reconnect_requested, 1);
    ensure_worker();
}

moonraker_state_t moonraker_state(void)
{
    return (moonraker_state_t)InterlockedCompareExchange(&state_value, 0, 0);
}
