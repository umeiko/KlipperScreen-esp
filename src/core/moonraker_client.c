/*
 * Moonraker WebSocket 客户端（esp32）。
 * 参考 KlipperScreen screen.py 握手链路的裁剪版（4 步，subscribe 响应即全量快照）：
 *   identify → server.info(等 klippy_connected) → objects.list → objects.subscribe
 * 断线指数退避 1→30s 无上限重连（无人值守，不用 KlipperScreen 的 4 次上限）。
 * 数据出口：status 字典文本经 lv_async_call 投递给 printer_model_apply_status_json。
 */
#include "moonraker_client.h"
#include "printer_model_internal.h"
#include "app_settings.h"
#include "version.h"
#include "bsp_wifi.h"
#include "bsp.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "lvgl.h"

#define TAG "moonraker"

#define WS_BUF_SIZE     8192    /* 订阅全量快照可达数 KB；docs §10 建议 >8KB */
#define RECONNECT_MAX_S 30
#define KLIPPY_RETRY_MS 5000
#define HEARTBEAT_MS    5000    /* 应用层心跳（兼测 RTT） */
#define PONG_TIMEOUT_S  15      /* 上位机死机（无 FIN）时最坏 ~ping+pong 超时发现 */
#define ZOMBIE_MS       20000   /* 超过此时间完全无 WS 流量（含 pong）= 僵尸连接，强制重建 */

typedef struct {
    int id;
    void (*cb)(cJSON *result);                   /* 内部握手用：WS 任务上下文，借用 msg 树 */
    void (*cb_json)(char *result_json, void *ud);/* 对外 moonraker_rpc：LVGL 上下文，堆字符串 */
    void *ud;
} pending_t;

static esp_websocket_client_handle_t ws;
static moonraker_state_t state = MOONRAKER_OFFLINE;
static moonraker_conf_t conf;
static bool             conf_valid;
static bool             started;            /* 客户端实例已创建 */
static esp_timer_handle_t reconnect_timer;
static esp_timer_handle_t klippy_timer;
static esp_timer_handle_t hb_timer;
static esp_timer_handle_t reload_timer;
static int64_t            hb_sent_us;
static int64_t            last_rx_ms;       /* 最近一次收到任何 WS 帧（含 ping/pong），僵尸检测用 */
static int              backoff_s = 1;

static int      next_id = 1;
static pending_t pending[8];

/* 断连/发送失败都会留下永远等不到应答的僵尸条目，必须在换连接时清空，
   否则 8 格占满后所有 RPC（含握手）被丢弃——曾现网导致重连后卡死在 CONNECTING */
static void clear_pending(void)
{
    memset(pending, 0, sizeof(pending));
}

/* WS 分片重组缓冲（payload_offset 累积） */
static char  *rx_buf;
static size_t rx_len;

static void handshake_step_subscribe(void);
static void force_reconnect(void);

/* ---------- LVGL 投递 ---------- */
static void apply_in_lvgl(void *p)
{
    printer_model_apply_status_json((char *)p);   /* 内部负责 free */
}

static void set_online_in_lvgl(void *p)
{
    printer_model_set_online((int)(intptr_t)p);
}

static void report_gcode_in_lvgl(void *p)
{
    printer_model_report_gcode_response((char *)p);   /* 内部负责 free */
}

static void report_rpc_err_in_lvgl(void *p)
{
    printer_model_report_rpc_error((char *)p);   /* 内部负责 free */
}

static void report_rtt_in_lvgl(void *p)
{
    printer_model_set_rtt((int)(intptr_t)p);
}

/* lv_async_call 会改 LVGL 全局 timer 链表；本文件均在 WS/esp_timer/cli 任务上下文
 * 调用它，必须持 LVGL 锁与 lvgl_task 的 lv_timer_handler 互斥——否则链表竞争会
 * 导致 async 定时器被执行两次/拿到野指针（曾现网崩溃：lv_async_timer_cb lv_free 断言） */
static void post_to_lvgl(lv_async_cb_t cb, void *p)
{
    bsp_lvgl_lock();
    lv_async_call(cb, p);
    bsp_lvgl_unlock();
}

/* status 子对象（cJSON，属于 msg 树）序列化后投递；调用方之后负责删 msg 树 */
static void post_status(cJSON *status_obj)
{
    if (!status_obj) return;
    char *txt = cJSON_PrintUnformatted(status_obj);
    if (!txt) return;
    char *heap = malloc(strlen(txt) + 1);
    if (!heap) { cJSON_free(txt); return; }
    strcpy(heap, txt);
    cJSON_free(txt);
    post_to_lvgl(apply_in_lvgl, heap);
}

/* ---------- 发送 ---------- */
static int alloc_pending(void (*cb)(cJSON *result),
                         void (*cb_json)(char *, void *), void *ud)
{
    for (int i = 0; i < 8; i++)
        if (pending[i].id == 0) {
            pending[i].id = next_id++;
            pending[i].cb = cb;
            pending[i].cb_json = cb_json;
            pending[i].ud = ud;
            return pending[i].id;
        }
    ESP_LOGW(TAG, "pending table full, drop request");
    return 0;
}

static void free_pending(int id)
{
    for (int i = 0; i < 8; i++)
        if (pending[i].id == id) { memset(&pending[i], 0, sizeof(pending[i])); return; }
}

static bool send_rpc_full(const char *method, const char *params_json,
                          void (*cb)(cJSON *result), void (*cb_json)(char *, void *), void *ud)
{
    if (!ws || !esp_websocket_client_is_connected(ws)) return false;
    int id = alloc_pending(cb, cb_json, ud);
    if (id == 0) return false;

    char frame[1024];
    int n;
    if (params_json)
        n = snprintf(frame, sizeof(frame),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                     id, method, params_json);
    else
        n = snprintf(frame, sizeof(frame),
                     "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}", id, method);
    if (n <= 0 || n >= (int)sizeof(frame)) {   /* 参数太长（订阅帧走 cJSON 组装，见下） */
        ESP_LOGE(TAG, "rpc frame too long: %s", method);
        free_pending(id);
        return false;
    }
    if (esp_websocket_client_send_text(ws, frame, n, pdMS_TO_TICKS(2000)) < 0) {
        free_pending(id);   /* 发送失败：应答永远不会来，立即回收槽位 */
        return false;
    }
    return true;
}

static bool send_rpc_cb(const char *method, const char *params_json, void (*cb)(cJSON *result))
{
    return send_rpc_full(method, params_json, cb, NULL, NULL);
}

bool moonraker_send_rpc(const char *method, const char *params_json)
{
    return send_rpc_cb(method, params_json, NULL);
}

/* moonraker_rpc 应答投递载体：WS 任务序列化后投到 LVGL 上下文执行回调 */
typedef struct {
    void (*cb)(char *result_json, void *ud);
    void *ud;
    char *json;   /* 堆字符串（可为 NULL=RPC 错误），回调负责 free */
} rpc_delivery_t;

static void deliver_in_lvgl(void *p)
{
    rpc_delivery_t *d = p;
    d->cb(d->json, d->ud);
    free(d);
}

bool moonraker_rpc(const char *method, const char *params_json,
                   void (*cb)(char *result_json, void *ud), void *ud)
{
    if (!cb) return false;
    return send_rpc_full(method, params_json, NULL, cb, ud);
}

/* ---------- 握手 ---------- */
static void on_subscribe_result(cJSON *result)
{
    cJSON *status = cJSON_GetObjectItem(result, "status");
    post_status(status);
    state = MOONRAKER_READY;
    post_to_lvgl(set_online_in_lvgl, (void *)1);
    ESP_LOGI(TAG, "subscribe done, READY");
    bsp_time_sync_from_host(conf.host, conf.port);   /* 内网对时：取 Moonraker HTTP Date */
}

static void handshake_step_subscribe(void)
{
    /* 订阅最小集（不订 configfile/bed_mesh/motion_report，对齐 docs §10 裁剪） */
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
    if (!send_rpc_cb("printer.objects.subscribe", params, on_subscribe_result))
        ESP_LOGW(TAG, "subscribe send failed");
}

static void on_objects_list_result(cJSON *result)
{
    (void)result;   /* v1 不做动态设备枚举，固定 extruder + heater_bed */
    handshake_step_subscribe();
}

static void query_server_info(void);

static void on_server_info(cJSON *result)
{
    cJSON *kc = cJSON_GetObjectItem(result, "klippy_connected");
    if (cJSON_IsBool(kc) && cJSON_IsTrue(kc)) {
        send_rpc_cb("printer.objects.list", NULL, on_objects_list_result);
    } else {
        /* klippy 还没起来（打印主机重启中）：5s 后重查，无上限 */
        ESP_LOGW(TAG, "klippy not connected, retry in %ds", KLIPPY_RETRY_MS / 1000);
        esp_timer_start_once(klippy_timer, KLIPPY_RETRY_MS * 1000);
    }
}

static void query_server_info_cb(void *arg)
{
    (void)arg;
    if (state != MOONRAKER_OFFLINE)
        send_rpc_cb("server.info", NULL, on_server_info);
}

static void query_server_info(void)
{
    send_rpc_cb("server.info", NULL, on_server_info);
}

/* ---------- 应用层心跳：READY 后每 5s 一次 server.info，RTT 即链路延迟。
   WS 协议层 ping/pong 之外的补充——pong 超时管「死连接」，这个管「延迟可见」 ---------- */
static void on_heartbeat_result(cJSON *result)
{
    (void)result;
    uint32_t ms = (uint32_t)((esp_timer_get_time() - hb_sent_us) / 1000);
    post_to_lvgl(report_rtt_in_lvgl, (void *)(intptr_t)ms);
}

static void heartbeat_cb(void *arg)
{
    (void)arg;
    if (state != MOONRAKER_READY) return;

    /* 僵尸检测：READY 状态下单向流（订阅推送 + pong）从不该断 20s。
       esp_websocket_client 对「对端优雅 FIN」有缺陷——recv 把 EOF 当超时永远空转，
       既不报 DISCONNECTED 也不回到 ping/pong 检查（实测 systemctl stop moonraker 后
       界面永远显示已连接）。组件不好改（registry 托管），在应用层兜底：强拆重建。 */
    if (last_rx_ms && esp_timer_get_time() / 1000 - last_rx_ms > ZOMBIE_MS) {
        ESP_LOGW(TAG, "no ws traffic for %dms, zombie connection, force reconnect", ZOMBIE_MS);
        force_reconnect();
        return;
    }
    hb_sent_us = esp_timer_get_time();
    send_rpc_cb("server.info", NULL, on_heartbeat_result);
}

static void handshake_begin(void)
{
    /* client_name 等字段对齐 server.connection.identify 约定（type=display）；
       版本号来自 version.h（KR_VERSION），与设置页显示一致 */
    send_rpc_cb("server.connection.identify",
                "{\"client_name\":\"klipper-remote-esp32\",\"version\":\"" KR_VERSION "\","
                "\"type\":\"display\",\"url\":\"https://github.com/klipper-remote\"}",
                NULL);
    query_server_info();
}

/* ---------- 通知 ---------- */
static void handle_notify(const char *method, cJSON *params)
{
    if (strcmp(method, "notify_status_update") == 0) {
        /* params = [ {object:{...}}, eventtime ] */
        cJSON *status = cJSON_GetArrayItem(params, 0);
        post_status(status);
    } else if (strcmp(method, "notify_gcode_response") == 0) {
        /* params = ["!! Endstop not triggered", eventtime]；只关心 "!!" 错误行，
           投递给模型存为待提示错误，UI 节拍弹 toast（无错误不上屏） */
        cJSON *s = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsString(s) && s->valuestring && strncmp(s->valuestring, "!!", 2) == 0) {
            char *heap = malloc(strlen(s->valuestring) + 1);
            if (heap) { strcpy(heap, s->valuestring); post_to_lvgl(report_gcode_in_lvgl, heap); }
        }
    } else if (strcmp(method, "notify_klippy_ready") == 0) {
        /* klippy 重启完成：重发订阅拿全新快照 */
        handshake_step_subscribe();
    } else if (strcmp(method, "notify_klippy_shutdown") == 0 ||
               strcmp(method, "notify_klippy_disconnected") == 0) {
        /* 伪造成 webhooks 状态合入模型（KlipperScreen notification_handler 同款手法） */
        const char *s = strcmp(method, "notify_klippy_shutdown") == 0
                        ? "{\"webhooks\":{\"state\":\"shutdown\"}}"
                        : "{\"webhooks\":{\"state\":\"disconnected\"}}";
        char *heap = malloc(strlen(s) + 1);
        if (heap) { strcpy(heap, s); post_to_lvgl(apply_in_lvgl, heap); }
    }
    /* notify_gcode_response 已转 toast；proc_stat / filelist_changed 等本期忽略 */
}

/* ---------- 收包 ---------- */
static void on_ws_message(const char *data, int len)
{
    cJSON *msg = cJSON_ParseWithLength(data, len);
    if (!msg) {
        ESP_LOGW(TAG, "bad json (%d bytes)", len);
        return;
    }

    cJSON *id = cJSON_GetObjectItem(msg, "id");
    if (cJSON_IsNumber(id)) {
        for (int i = 0; i < 8; i++) {
            if (pending[i].id == (int)id->valuedouble) {
                void (*cb)(cJSON *) = pending[i].cb;
                void (*cb_json)(char *, void *) = pending[i].cb_json;
                void *ud = pending[i].ud;
                memset(&pending[i], 0, sizeof(pending[i]));
                cJSON *err = cJSON_GetObjectItem(msg, "error");
                if (err) {
                    cJSON *em = cJSON_GetObjectItem(err, "message");
                    const char *emsg = cJSON_IsString(em) ? em->valuestring : "rpc error";
                    ESP_LOGW(TAG, "rpc id=%d error: %s", (int)id->valuedouble, emsg);
                    /* 无主请求的 RPC 错误基本是用户按键触发（点动未归位/超程等），
                     * 不会走 gcode "!!" 行，必须在这里弹 toast */
                    if (!cb && !cb_json) {
                        char *heap = malloc(strlen(emsg) + 1);
                        if (heap) { strcpy(heap, emsg); post_to_lvgl(report_rpc_err_in_lvgl, heap); }
                    }
                    if (cb_json) {   /* 对外回调也要收到失败信号（NULL） */
                        rpc_delivery_t *d = malloc(sizeof(*d));
                        if (d) {
                            d->cb = cb_json; d->ud = ud; d->json = NULL;
                            post_to_lvgl(deliver_in_lvgl, d);
                        }
                    }
                } else if (cb) {
                    cb(cJSON_GetObjectItem(msg, "result"));
                } else if (cb_json) {
                    cJSON *result = cJSON_GetObjectItem(msg, "result");
                    char *txt = result ? cJSON_PrintUnformatted(result) : NULL;
                    rpc_delivery_t *d = malloc(sizeof(*d));
                    if (d) {
                        d->cb = cb_json; d->ud = ud; d->json = txt;
                        post_to_lvgl(deliver_in_lvgl, d);
                    } else {
                        cJSON_free(txt);
                    }
                }
                break;
            }
        }
    } else {
        cJSON *m = cJSON_GetObjectItem(msg, "method");
        if (cJSON_IsString(m))
            handle_notify(m->valuestring, cJSON_GetObjectItem(msg, "params"));
    }
    cJSON_Delete(msg);
}

/* ---------- 重连 ---------- */
static void reconnect_cb(void *arg)
{
    (void)arg;
    if (state == MOONRAKER_OFFLINE && started) {
        ESP_LOGI(TAG, "reconnecting to %s:%u", conf.host, (unsigned)conf.port);
        state = MOONRAKER_CONNECTING;
        esp_websocket_client_start(ws);
    }
}

static void schedule_reconnect(void)
{
    if (state == MOONRAKER_OFFLINE) return;   /* CLOSED/FINISH 会连发，只调度一次 */
    state = MOONRAKER_OFFLINE;
    clear_pending();
    post_to_lvgl(set_online_in_lvgl, (void *)0);
    if (!conf_valid) return;
    ESP_LOGI(TAG, "disconnected, retry in %ds", backoff_s);
    esp_timer_start_once(reconnect_timer, (uint64_t)backoff_s * 1000000);
    backoff_s = backoff_s * 2 > RECONNECT_MAX_S ? RECONNECT_MAX_S : backoff_s * 2;
}

/* ---------- WS 事件 ---------- */
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *ev = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        backoff_s = 1;
        last_rx_ms = esp_timer_get_time() / 1000;
        state = MOONRAKER_CONNECTING;
        handshake_begin();
        break;
    case WEBSOCKET_EVENT_DATA:
        last_rx_ms = esp_timer_get_time() / 1000;   /* 任何帧（含 pong）都算活着 */
        if (ev->op_code != 1 && ev->op_code != 0) break;   /* 只处理 text/continuation */
        if (ev->payload_offset == 0) {
            free(rx_buf);
            rx_buf = malloc(ev->payload_len + 1);
            rx_len = 0;
            if (!rx_buf) break;
        }
        if (rx_buf && ev->payload_offset + ev->data_len <= ev->payload_len) {
            memcpy(rx_buf + ev->payload_offset, ev->data_ptr, ev->data_len);
            rx_len = ev->payload_offset + ev->data_len;
            if (rx_len >= ev->payload_len) {
                rx_buf[rx_len] = 0;
                on_ws_message(rx_buf, (int)rx_len);
                free(rx_buf);
                rx_buf = NULL;
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ws disconnected");
        schedule_reconnect();
        break;
    /* 对端优雅关闭（如 systemctl stop moonraker：tornado 发 CLOSE 帧）时组件走
       CLOSING→任务退出，只发 CLOSED/FINISH，永远不发 DISCONNECTED——不接这两个事件
       客户端就成永久僵尸（实测：状态卡 READY、温度不更新、控制静默失败） */
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "ws closed by server");
        schedule_reconnect();
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGW(TAG, "ws task finished");
        schedule_reconnect();
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default:
        break;
    }
}

/* ---------- 对外 ---------- */
static void reload_cb(void *arg);   /* moonraker_reload 的实际工作，esp_timer 上下文 */

static void ensure_timers(void)
{
    if (!reconnect_timer) {
        esp_timer_create_args_t a1 = {.callback = reconnect_cb, .name = "mr_reconn"};
        esp_timer_create(&a1, &reconnect_timer);
    }
    if (!klippy_timer) {
        esp_timer_create_args_t a2 = {.callback = query_server_info_cb, .name = "mr_klippy"};
        esp_timer_create(&a2, &klippy_timer);
    }
    if (!hb_timer) {
        esp_timer_create_args_t a3 = {.callback = heartbeat_cb, .name = "mr_hb"};
        esp_timer_create(&a3, &hb_timer);
        esp_timer_start_periodic(hb_timer, HEARTBEAT_MS * 1000);
    }
    if (!reload_timer) {
        esp_timer_create_args_t a4 = {.callback = reload_cb, .name = "mr_reload"};
        esp_timer_create(&a4, &reload_timer);
    }
}

static void destroy_client(void)
{
    if (!ws) return;
    /* close 内部会等 close 握手，僵尸连接上可能阻塞到 network_timeout；
       这里（esp_timer 任务上下文）用 1s 上限，不等它优雅 */
    esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    ws = NULL;
    started = false;
}

static void force_reconnect(void)
{
    destroy_client();
    state = MOONRAKER_OFFLINE;
    backoff_s = 1;
    last_rx_ms = 0;
    clear_pending();
    post_to_lvgl(set_online_in_lvgl, (void *)0);
    moonraker_start();   /* 立即重连（不等退避），WiFi 还在就秒回 */
}

void moonraker_start(void)
{
    if (started) return;
    ensure_timers();
    if (!conf_valid) {
        conf_valid = settings_load_moonraker(&conf);
        if (!conf_valid) return;   /* 未配置，等面板里设置后 reload */
    }
    if (!bsp_wifi_connected()) return;   /* 等 WiFi，model 的 2s 轮询会再进来 */
    ESP_LOGI(TAG, "conf: host=%s port=%u", conf.host, (unsigned)conf.port);

    char uri[160];
    if (conf.api_key[0])
        snprintf(uri, sizeof(uri), "ws://%s:%u/websocket?token=%s",
                 conf.host, (unsigned)conf.port, conf.api_key);
    else
        snprintf(uri, sizeof(uri), "ws://%s:%u/websocket", conf.host, (unsigned)conf.port);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = WS_BUF_SIZE,
        .task_stack = 6144,
        .disable_auto_reconnect = true,   /* 不用内置重连，自己做指数退避 */
        .network_timeout_ms = 8000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = PONG_TIMEOUT_S,   /* 默认 120s 太肉，收紧到 15s */
    };
    ESP_LOGI(TAG, "connecting %s", uri);
    state = MOONRAKER_CONNECTING;
    ws = esp_websocket_client_init(&cfg);   /* registry 版 v1.x 直接返回句柄 */
    if (!ws) {
        state = MOONRAKER_OFFLINE;
        return;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    started = true;
    esp_websocket_client_start(ws);
}

/* 实际重连逻辑。只能在非 LVGL 上下文跑：destroy_client 会阻塞等 close 握手，
   post_to_lvgl 要拿 LVGL 锁——若在 LVGL 任务里直接调，锁重入即死锁（点击事件
   处理正持锁）。所以 moonraker_reload 只做调度，工作挪到 esp_timer 任务里。 */
static void reload_cb(void *arg)
{
    (void)arg;
    memset(&conf, 0, sizeof(conf));
    conf_valid = settings_load_moonraker(&conf);
    esp_timer_stop(reconnect_timer);
    esp_timer_stop(klippy_timer);
    backoff_s = 1;
    destroy_client();
    state = MOONRAKER_OFFLINE;
    post_to_lvgl(set_online_in_lvgl, (void *)0);
    moonraker_start();
}

void moonraker_reload(void)
{
    ensure_timers();
    esp_timer_stop(reload_timer);            /* 连点多次只执行最后一次 */
    esp_timer_start_once(reload_timer, 20000);   /* 20ms 后在 esp_timer 任务执行 */
}

moonraker_state_t moonraker_state(void)
{
    return state;
}
