/*
 * Compact Moonraker backend for the Wio Terminal.
 *
 * The SAMD51 has only 496 KiB of executable flash, so pulling the ESP-IDF
 * websocket/cJSON stack is not an option.  This implementation uses the
 * RTL8720DN's WiFiClient, a small RFC6455 state machine, and extracts only the
 * fields used by the core status/control panels.
 */
#include <Arduino.h>
#include <Seeed_Arduino_FreeRTOS.h>
#include <WiFiClient.h>

extern "C" {
#include "app_settings.h"
#include "bsp.h"
#include "bsp_wifi.h"
#include "moonraker_client.h"
#include "printer.h"
#include "lvgl.h"
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_wio.h"

namespace {

struct PrinterState {
    printer_state_t state;
    float ext;
    float bed;
    float ext_target;
    float bed_target;
    float pos[3];
    bool homed[3];
    int progress;
    uint32_t elapsed;
    int layer;
    int layers;
    char filename[96];
    char error[96];
};

static PrinterState p = {
    PRINTER_STATE_DISCONNECTED, 0, 0, 0, 0, {0, 0, 0},
    {false, false, false}, 0, 0, 0, 0, "", ""
};
static void (*refresh_hook)(void);

enum LinkState { LINK_OFF, LINK_WAIT_WIFI, LINK_CONNECT, LINK_HTTP, LINK_WS };
static LinkState link_state = LINK_OFF;
static moonraker_state_t mr_state = MOONRAKER_OFFLINE;
static WiFiClient client;
static bool disabled;
static QueueHandle_t tx_queue;
static TaskHandle_t network_task_handle;
static volatile uint32_t network_epoch;
static uint32_t retry_at;
static uint32_t last_ping;
static uint32_t http_deadline;
static uint32_t next_id = 1;
static char http_header[512];
static size_t http_len;

static uint8_t frame_stage;
static uint8_t frame_opcode;
static bool frame_fin;
static bool frame_masked;
static uint8_t frame_ext_left;
static uint8_t frame_mask_pos;
static uint8_t frame_mask[4];
static uint64_t frame_len;
static uint64_t frame_pos;
static char message[16384];
static size_t message_len;
static bool message_text;
static bool message_overflow;
static uint8_t control_payload[125];
static volatile uint32_t tx_message_count;
static volatile uint32_t rx_message_count;
static volatile size_t last_rx_message_len;
static volatile bool last_rx_overflow;

struct FileRequest {
    volatile bool active;
    volatile bool ready;
    unsigned id;
    unsigned offset;
    unsigned generation;
    unsigned result_generation;
    uint32_t started;
    printer_files_cb cb;
    void *ud;
    printer_file_page_t page;
};
static FileRequest file_req;

static void sync_time_from_handshake(void)
{
    char *line = strstr(http_header, "\r\nDate:");
    if (!line) return;
    line += 7;
    while (*line == ' ' || *line == '\t') ++line;
    char *end = strstr(line, "\r\n");
    if (!end) return;
    char saved = *end;
    *end = 0;
    (void)bsp_time_sync_from_http_date(line);
    *end = saved;
}

static void discard_tx_queue(void)
{
    if (!tx_queue) return;
    char *text = nullptr;
    while (xQueueReceive(tx_queue, &text, 0) == pdTRUE) free(text);
}

static void mark_offline(void)
{
    client.stop();
    discard_tx_queue();
    link_state = disabled ? LINK_OFF : LINK_WAIT_WIFI;
    mr_state = MOONRAKER_OFFLINE;
    p.state = PRINTER_STATE_DISCONNECTED;
    retry_at = millis() + 3000;
    http_deadline = 0;
    http_len = 0;
    frame_stage = 0;
    message_len = 0;
    message_text = false;
    message_overflow = false;
}

static const char *skip_ws(const char *s, const char *end)
{
    while (s < end && isspace((unsigned char)*s)) ++s;
    return s;
}

static bool object_slice(const char *json, const char *name,
                         const char **begin, const char **end)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", name);
    const char *key = strstr(json, needle);
    if (!key) return false;
    const char *s = strchr(key + strlen(needle), ':');
    if (!s) return false;
    const char *limit = json + strlen(json);
    s = skip_ws(s + 1, limit);
    if (s >= limit || *s != '{') return false;

    int depth = 0;
    bool quoted = false, escaped = false;
    for (const char *q = s; q < limit; ++q) {
        char c = *q;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) {
            *begin = s;
            *end = q + 1;
            return true;
        }
    }
    return false;
}

static const char *find_key(const char *begin, const char *end, const char *key)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pkey = begin;
    while ((pkey = strstr(pkey, needle)) != nullptr && pkey < end) {
        const char *v = strchr(pkey + strlen(needle), ':');
        if (v && v < end) return skip_ws(v + 1, end);
        pkey += strlen(needle);
    }
    return nullptr;
}

static bool get_number(const char *begin, const char *end,
                       const char *key, float *out)
{
    const char *v = find_key(begin, end, key);
    if (!v || v >= end) return false;
    char *tail;
    double n = strtod(v, &tail);
    if (tail == v || tail > end) return false;
    *out = (float)n;
    return true;
}

static bool get_double(const char *begin, const char *end,
                       const char *key, double *out)
{
    const char *v = find_key(begin, end, key);
    if (!v || v >= end) return false;
    char *tail;
    double n = strtod(v, &tail);
    if (tail == v || tail > end) return false;
    *out = n;
    return true;
}

static bool get_string(const char *begin, const char *end, const char *key,
                       char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    const char *v = find_key(begin, end, key);
    if (!v || v >= end || *v != '"') return false;
    ++v;
    size_t n = 0;
    bool escaped = false;
    while (v < end && *v) {
        char c = *v++;
        if (!escaped && c == '"') break;
        if (!escaped && c == '\\') { escaped = true; continue; }
        if (escaped) {
            if (c == 'n' || c == 'r' || c == 't') c = ' ';
            escaped = false;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return true;
}

static bool get_string_bounded(const char *begin, const char *end,
                               const char *key, char *out, size_t cap,
                               bool *truncated)
{
    if (!out || !cap) return false;
    const char *v = find_key(begin, end, key);
    if (!v || v >= end || *v != '"') return false;
    ++v;
    size_t n = 0;
    bool escaped = false, closed = false;
    if (truncated) *truncated = false;
    while (v < end) {
        char c = *v++;
        if (!escaped && c == '"') { closed = true; break; }
        if (!escaped && c == '\\') { escaped = true; continue; }
        if (escaped) {
            if (c == 'n' || c == 'r' || c == 't') c = ' ';
            escaped = false;
        }
        if (n + 1 < cap) out[n++] = c;
        else if (truncated) *truncated = true;
    }
    out[n] = 0;
    return closed;
}

static bool parse_file_list(const char *json, unsigned offset,
                            printer_file_page_t *page)
{
    memset(page, 0, sizeof(*page));
    const char *end = json + strlen(json);
    const char *result = strstr(json, "\"result\"");
    if (!result) return false;
    const char *pcur = strchr(result + 8, ':');
    if (!pcur) return false;
    pcur = skip_ws(pcur + 1, end);
    if (pcur >= end || *pcur++ != '[') return false;

    for (;;) {
        pcur = skip_ws(pcur, end);
        if (pcur >= end) return false;
        if (*pcur == ']') return true;
        if (*pcur == ',') { ++pcur; continue; }
        if (*pcur != '{') return false;

        const char *record = pcur;
        int depth = 0;
        bool quoted = false, escaped = false;
        for (; pcur < end; ++pcur) {
            char c = *pcur;
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { ++pcur; break; }
        }
        if (depth != 0) return false;

        char path[sizeof(page->files[0].name)];
        bool truncated = false;
        double size_value = -1, modified_value = 0;
        bool valid = get_string_bounded(record, pcur, "path", path,
                                        sizeof(path), &truncated) &&
                     !truncated && path[0] && path[0] != '.' &&
                     get_double(record, pcur, "size", &size_value) &&
                     size_value >= 0 && size_value <= UINT32_MAX;
        (void)get_double(record, pcur, "modified", &modified_value);
        if (truncated) {
            ++page->skipped;
        } else if (valid) {
            if (page->total >= offset &&
                page->count < PRINTER_FILES_PAGE_SIZE) {
                printer_file_t *file = &page->files[page->count++];
                snprintf(file->name, sizeof(file->name), "%s", path);
                file->size = (uint32_t)size_value;
                file->modified = modified_value;
            }
            ++page->total;
        }
    }
}

static bool get_vec3(const char *begin, const char *end, const char *key,
                     float out[3])
{
    const char *v = find_key(begin, end, key);
    if (!v || v >= end || *v != '[') return false;
    ++v;
    for (int i = 0; i < 3; ++i) {
        v = skip_ws(v, end);
        char *tail;
        out[i] = (float)strtod(v, &tail);
        if (tail == v || tail > end) return false;
        v = skip_ws(tail, end);
        if (i < 2) {
            if (v >= end || *v != ',') return false;
            ++v;
        }
    }
    return true;
}

static void apply_status(const char *json)
{
    const char *b, *e;
    float n;
    if (object_slice(json, "extruder", &b, &e)) {
        if (get_number(b, e, "temperature", &n)) p.ext = n;
        if (get_number(b, e, "target", &n)) p.ext_target = n;
    }
    if (object_slice(json, "heater_bed", &b, &e)) {
        if (get_number(b, e, "temperature", &n)) p.bed = n;
        if (get_number(b, e, "target", &n)) p.bed_target = n;
    }
    if (object_slice(json, "toolhead", &b, &e)) {
        (void)get_vec3(b, e, "position", p.pos);
        char homed[8];
        if (get_string(b, e, "homed_axes", homed, sizeof(homed))) {
            p.homed[0] = strchr(homed, 'x') != nullptr;
            p.homed[1] = strchr(homed, 'y') != nullptr;
            p.homed[2] = strchr(homed, 'z') != nullptr;
        }
    }
    if (object_slice(json, "display_status", &b, &e) &&
        get_number(b, e, "progress", &n))
        p.progress = constrain((int)lroundf(n * 1000.0f), 0, 1000);
    if (object_slice(json, "virtual_sdcard", &b, &e) &&
        get_number(b, e, "progress", &n))
        p.progress = constrain((int)lroundf(n * 1000.0f), 0, 1000);
    if (object_slice(json, "print_stats", &b, &e)) {
        char state[24];
        if (get_string(b, e, "state", state, sizeof(state))) {
            if (!strcmp(state, "printing")) p.state = PRINTER_STATE_PRINTING;
            else if (!strcmp(state, "paused")) p.state = PRINTER_STATE_PAUSED;
            else if (!strcmp(state, "complete")) p.state = PRINTER_STATE_COMPLETE;
            else if (!strcmp(state, "error")) p.state = PRINTER_STATE_ERROR;
            else p.state = PRINTER_STATE_STANDBY;
        }
        (void)get_string(b, e, "filename", p.filename, sizeof(p.filename));
        if (get_number(b, e, "print_duration", &n)) p.elapsed = (uint32_t)n;
        const char *ib, *ie;
        if (object_slice(b, "info", &ib, &ie)) {
            if (get_number(ib, ie, "current_layer", &n)) p.layer = (int)n;
            if (get_number(ib, ie, "total_layer", &n)) p.layers = (int)n;
        }
    }
    if (object_slice(json, "webhooks", &b, &e)) {
        char state[24];
        if (get_string(b, e, "state", state, sizeof(state)) &&
            (!strcmp(state, "error") || !strcmp(state, "shutdown")))
            p.state = PRINTER_STATE_ERROR;
    }
}

static size_t b64_16(const uint8_t in[16], char out[25])
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (int i = 0; i < 15; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = tab[(v >> 18) & 63]; out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];  out[o++] = tab[v & 63];
    }
    uint32_t v = (uint32_t)in[15] << 16;
    out[o++] = tab[(v >> 18) & 63]; out[o++] = tab[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '='; out[o] = 0;
    return o;
}

static bool send_frame(uint8_t opcode, const uint8_t *payload, size_t len)
{
    if (!client.connected() || len > 65535) return false;
    uint8_t hdr[8];
    size_t h = 0;
    hdr[h++] = 0x80 | opcode;
    if (len < 126) hdr[h++] = 0x80 | (uint8_t)len;
    else {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    }
    uint32_t mask = (uint32_t)random(0x7fffffffL) ^ millis();
    memcpy(hdr + h, &mask, 4); h += 4;
    if (client.write(hdr, h) != h) return false;
    uint8_t chunk[96];
    size_t sent = 0;
    const uint8_t *m = reinterpret_cast<const uint8_t *>(&mask);
    while (sent < len) {
        size_t n = min(sizeof(chunk), len - sent);
        for (size_t i = 0; i < n; ++i)
            chunk[i] = payload[sent + i] ^ m[(sent + i) & 3];
        if (client.write(chunk, n) != n) return false;
        sent += n;
    }
    return true;
}

static bool send_text_direct(const char *text)
{
    return text && send_frame(1, reinterpret_cast<const uint8_t *>(text), strlen(text));
}

static bool queue_text(const char *text)
{
    if (!tx_queue || !text || !text[0]) return false;
    size_t len = strlen(text) + 1;
    char *copy = static_cast<char *>(malloc(len));
    if (!copy) return false;
    memcpy(copy, text, len);
    if (xQueueSend(tx_queue, &copy, 0) != pdTRUE) {
        free(copy);
        return false;
    }
    return true;
}

static bool queue_rpc_text(const char *method, const char *params)
{
    char msg[640];
    unsigned long id = next_id++;
    int n = params && params[0]
        ? snprintf(msg, sizeof(msg),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%lu}",
                   method, params, id)
        : snprintf(msg, sizeof(msg),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%lu}",
                   method, id);
    return n > 0 && n < (int)sizeof(msg) && queue_text(msg);
}

static bool send_rpc_text(const char *method, const char *params)
{
    /* UI callbacks never touch WiFiClient directly. Every socket operation is
       owned by network_worker, otherwise one lost RTL8720DN RPC reply freezes
       LVGL for the full transport timeout. */
    return mr_state == MOONRAKER_READY && queue_rpc_text(method, params);
}

static void send_startup_rpc(void)
{
    char identify[256];
    snprintf(identify, sizeof(identify),
             "{\"jsonrpc\":\"2.0\",\"method\":\"server.connection.identify\","
             "\"params\":{\"client_name\":\"KlipperScreen-esp-Wio\","
             "\"version\":\"wio\",\"type\":\"display\","
             "\"url\":\"https://github.com/umeiko/KlipperScreen-esp\"},\"id\":%lu}",
             (unsigned long)next_id++);
    bool identify_queued = queue_text(identify);
    static const char subscribe[] =
        "{\"objects\":{"
        "\"extruder\":[\"temperature\",\"target\"],"
        "\"heater_bed\":[\"temperature\",\"target\"],"
        "\"toolhead\":[\"position\",\"homed_axes\"],"
        "\"print_stats\":[\"state\",\"filename\",\"print_duration\",\"info\"],"
        "\"display_status\":[\"progress\"],"
        "\"virtual_sdcard\":[\"progress\"],"
        "\"webhooks\":[\"state\",\"state_message\"]}}";
    bool subscribe_queued = queue_rpc_text("printer.objects.subscribe", subscribe);
    if (!identify_queued || !subscribe_queued)
        Serial.println("Moonraker: unable to queue startup RPC");
}

static void handle_message(char *json)
{
    ++rx_message_count;
    last_rx_message_len = strlen(json);
    last_rx_overflow = false;
    printer_state_t old_state = p.state;
    apply_status(json);

    if (file_req.active) {
        float id_value = 0;
        const char *json_end = json + strlen(json);
        if (get_number(json, json_end, "id", &id_value) &&
            (unsigned)id_value == file_req.id) {
            unsigned generation = file_req.generation;
            bool ok = parse_file_list(json, file_req.offset, &file_req.page);
            if (!ok) file_req.page.count = -1;
            if (file_req.active && file_req.generation == generation) {
                file_req.result_generation = generation;
                __DMB();
                file_req.ready = true;
                file_req.active = false;
            }
        }
    }
    if (old_state == PRINTER_STATE_DISCONNECTED &&
        p.state != PRINTER_STATE_DISCONNECTED) {
        Serial.print("Moonraker: printer state ready=");
        Serial.println((int)p.state);
    }
    if (strstr(json, "printer.objects.subscribe") == nullptr &&
        strstr(json, "\"result\"") == nullptr &&
        strstr(json, "notify_status_update") == nullptr)
        return;
    if (mr_state != MOONRAKER_READY) Serial.println("Moonraker: subscribed");
    mr_state = MOONRAKER_READY;
}

static void reset_frame(void)
{
    frame_stage = 0;
    frame_len = frame_pos = 0;
    frame_ext_left = frame_mask_pos = 0;
}

static void complete_frame(void)
{
    if (frame_opcode == 8) { mark_offline(); return; }
    if (frame_opcode == 9) {
        (void)send_frame(10, control_payload, (size_t)frame_len);
    } else if (frame_opcode == 1 || frame_opcode == 0) {
        if (frame_fin) {
            if (!message_overflow) {
                message[message_len] = 0;
                handle_message(message);
            } else {
                last_rx_message_len = message_len;
                last_rx_overflow = true;
                Serial.println("Moonraker: incoming message overflow");
                if (file_req.active) {
                    file_req.page.count = -1;
                    file_req.result_generation = file_req.generation;
                    __DMB();
                    file_req.ready = true;
                    file_req.active = false;
                }
            }
            message_len = 0;
            message_text = false;
            message_overflow = false;
        }
    }
    reset_frame();
}

static void consume_ws_byte(uint8_t c)
{
    switch (frame_stage) {
    case 0:
        frame_fin = (c & 0x80) != 0;
        frame_opcode = c & 0x0f;
        if (frame_opcode == 1) { message_len = 0; message_text = true; message_overflow = false; }
        frame_stage = 1;
        break;
    case 1:
        frame_masked = (c & 0x80) != 0;
        frame_len = c & 0x7f;
        if (frame_len == 126) { frame_len = 0; frame_ext_left = 2; frame_stage = 2; }
        else if (frame_len == 127) { frame_len = 0; frame_ext_left = 8; frame_stage = 2; }
        else if (frame_masked) { frame_mask_pos = 0; frame_stage = 3; }
        else { frame_stage = 4; if (!frame_len) complete_frame(); }
        break;
    case 2:
        frame_len = (frame_len << 8) | c;
        if (--frame_ext_left == 0) {
            if (frame_masked) { frame_mask_pos = 0; frame_stage = 3; }
            else { frame_stage = 4; if (!frame_len) complete_frame(); }
        }
        break;
    case 3:
        frame_mask[frame_mask_pos++] = c;
        if (frame_mask_pos == 4) { frame_stage = 4; if (!frame_len) complete_frame(); }
        break;
    default: {
        uint8_t value = frame_masked ? (c ^ frame_mask[frame_pos & 3]) : c;
        if ((frame_opcode == 1 || frame_opcode == 0) && message_text) {
            if (message_len + 1 < sizeof(message)) message[message_len++] = (char)value;
            else message_overflow = true;
        } else if (frame_opcode == 9 && frame_pos < sizeof(control_payload)) {
            control_payload[frame_pos] = value;
        }
        if (++frame_pos >= frame_len) complete_frame();
        break;
    }
    }
}

static void start_connection(void)
{
    moonraker_conf_t conf;
    if (!settings_load_moonraker(&conf) || !conf.valid || !conf.host[0]) {
        retry_at = millis() + 3000;
        return;
    }
    mr_state = MOONRAKER_CONNECTING;
    Serial.print("Moonraker: connecting to ");
    Serial.print(conf.host);
    Serial.print(":");
    Serial.println(conf.port ? conf.port : 7125);
    client.stop();
    IPAddress address;
    int connected = address.fromString(conf.host)
        ? client.connect(address, conf.port ? conf.port : 7125, 800)
        : client.connect(conf.host, conf.port ? conf.port : 7125, 800);
    if (!connected) {
        mark_offline();
        return;
    }
    client.setTimeout(1);
    uint8_t raw[16];
    for (uint8_t &v : raw) v = (uint8_t)random(256);
    char key[25];
    b64_16(raw, key);
    char request[512];
    const char *token_prefix = conf.api_key[0] ? "?token=" : "";
    const char *token = conf.api_key[0] ? conf.api_key : "";
    int n = snprintf(request, sizeof(request),
        "GET /websocket%s%s HTTP/1.1\r\nHost: %s:%u\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        token_prefix, token, conf.host, conf.port ? conf.port : 7125, key);
    if (n <= 0 || n >= (int)sizeof(request) ||
        client.write(reinterpret_cast<const uint8_t *>(request), n) != (size_t)n) {
        mark_offline();
        return;
    }
    http_len = 0;
    http_deadline = millis() + 3000;
    link_state = LINK_HTTP;
}

static void drain_tx_queue(void)
{
    if (!tx_queue || link_state != LINK_WS) return;
    char *text = nullptr;
    while (xQueueReceive(tx_queue, &text, 0) == pdTRUE) {
        bool ok = send_text_direct(text);
        free(text);
        if (!ok) {
            Serial.println("Moonraker: websocket send failed");
            discard_tx_queue();
            mark_offline();
            return;
        }
        ++tx_message_count;
    }
}

static void network_poll(void)
{
    if (disabled) return;
    uint32_t now = millis();
    if (!bsp_wifi_connected()) {
        if (link_state != LINK_WAIT_WIFI) mark_offline();
        return;
    }
    if (link_state == LINK_OFF || link_state == LINK_WAIT_WIFI || link_state == LINK_CONNECT) {
        if ((int32_t)(now - retry_at) >= 0) start_connection();
        return;
    }
    if (!client.connected()) { mark_offline(); return; }
    if (link_state == LINK_HTTP &&
        (int32_t)(now - http_deadline) >= 0) {
        Serial.println("Moonraker: websocket handshake timeout");
        mark_offline();
        return;
    }

    int budget = 1200;
    while (budget-- > 0 && client.available()) {
        int ch = client.read();
        if (ch < 0) break;
        if (link_state == LINK_HTTP) {
            if (http_len + 1 >= sizeof(http_header)) { mark_offline(); return; }
            http_header[http_len++] = (char)ch;
            http_header[http_len] = 0;
            if (http_len >= 4 && !strcmp(http_header + http_len - 4, "\r\n\r\n")) {
                if (strncmp(http_header, "HTTP/1.1 101", 12) &&
                    strncmp(http_header, "HTTP/1.0 101", 12)) {
                    mark_offline(); return;
                }
                link_state = LINK_WS;
                http_deadline = 0;
                Serial.println("Moonraker: websocket ready");
                sync_time_from_handshake();
                reset_frame();
                send_startup_rpc();
                /* The authenticated WebSocket transport is usable now. The
                   subscription response will populate p immediately after. */
                mr_state = MOONRAKER_READY;
                last_ping = now;
            }
        } else {
            consume_ws_byte((uint8_t)ch);
        }
    }
    drain_tx_queue();
    if (link_state == LINK_WS && now - last_ping > 15000) {
        uint32_t stamp = now;
        if (!send_frame(9, reinterpret_cast<uint8_t *>(&stamp), sizeof(stamp)))
            mark_offline();
        last_ping = now;
    }
}

static void network_worker(void *)
{
    uint32_t seen_epoch = network_epoch;
    bool paused_for_scan = false;
    for (;;) {
        if (bsp_wio_wifi_scan_requested()) {
            if (!paused_for_scan) {
                discard_tx_queue();
                mark_offline();
                paused_for_scan = true;
            }
            bsp_wio_wifi_network_idle(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (paused_for_scan) {
            paused_for_scan = false;
            retry_at = 0;
            link_state = disabled ? LINK_OFF : LINK_WAIT_WIFI;
        }
        bsp_wio_wifi_network_idle(false);

        uint32_t requested_epoch = network_epoch;
        if (requested_epoch != seen_epoch) {
            seen_epoch = requested_epoch;
            discard_tx_queue();
            mark_offline();
            if (!disabled) {
                retry_at = 0;
                link_state = LINK_WAIT_WIFI;
            }
        }
        network_poll();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void timer_1s(lv_timer_t *) { if (refresh_hook) refresh_hook(); }

static void gcode(const char *script)
{
    if (!script || mr_state != MOONRAKER_READY) return;
    char escaped[300];
    size_t n = 0;
    for (const char *s = script; *s && n + 2 < sizeof(escaped); ++s) {
        if (*s == '"' || *s == '\\') escaped[n++] = '\\';
        escaped[n++] = *s;
    }
    escaped[n] = 0;
    char params[340];
    snprintf(params, sizeof(params), "{\"script\":\"%s\"}", escaped);
    (void)send_rpc_text("printer.gcode.script", params);
}

} // namespace

extern "C" {

void printer_set_refresh_hook(void (*fn)(void)) { refresh_hook = fn; }
void printer_init(void)
{
    lv_timer_create(timer_1s, 1000, nullptr);
    tx_queue = xQueueCreate(4, sizeof(char *));
    moonraker_start();
    if (!tx_queue ||
        xTaskCreate(network_worker, "moonraker", 1536, nullptr, 2,
                    &network_task_handle) != pdPASS) {
        Serial.println("Moonraker: unable to allocate network worker");
        mr_state = MOONRAKER_OFFLINE;
        p.state = PRINTER_STATE_DISCONNECTED;
    }
}

printer_capabilities_t printer_capabilities(void)
{
    if (mr_state != MOONRAKER_READY) return 0;
    return PRINTER_CAP_TEMP_CONTROL | PRINTER_CAP_MOVE | PRINTER_CAP_EXTRUDE |
           PRINTER_CAP_FILES | PRINTER_CAP_PRINT_START |
           PRINTER_CAP_PAUSE | PRINTER_CAP_RESUME | PRINTER_CAP_CANCEL |
           PRINTER_CAP_EMERGENCY_STOP | PRINTER_CAP_FIRMWARE_RESTART;
}
printer_state_t printer_state(void) { return p.state; }
float printer_temp_ext(void) { return p.ext; }
float printer_temp_bed(void) { return p.bed; }
float printer_target_ext(void) { return p.ext_target; }
float printer_target_bed(void) { return p.bed_target; }
float printer_pos(int axis) { return axis >= 0 && axis < 3 ? p.pos[axis] : 0; }
int printer_homed(int axis) { return axis >= 0 && axis < 3 && p.homed[axis]; }
int printer_progress_permille(void) { return p.progress; }
const char *printer_filename(void) { return p.filename; }
uint32_t printer_print_elapsed_s(void) { return p.elapsed; }
uint32_t printer_print_eta_s(void)
{
    return p.progress > 0 && p.progress < 1000
        ? (uint32_t)((uint64_t)p.elapsed * (1000 - p.progress) / p.progress) : 0;
}
int printer_layer_current(void) { return p.layer; }
int printer_layer_total(void) { return p.layers; }
float printer_flow_pct(void) { return 100.0f; }
int printer_rtt_ms(void) { return 0; }
bool printer_take_error(char *out, size_t cap)
{
    if (!p.error[0] || !out || !cap) return false;
    snprintf(out, cap, "%s", p.error); p.error[0] = 0; return true;
}

void printer_set_target_ext(float t) { char c[32]; snprintf(c, sizeof(c), "M104 S%.0f", t); gcode(c); }
void printer_set_target_bed(float t) { char c[32]; snprintf(c, sizeof(c), "M140 S%.0f", t); gcode(c); }
void printer_jog(int axis, float dist)
{
    static const char names[] = "XYZ";
    if (axis < 0 || axis > 2) return;
    char c[80]; snprintf(c, sizeof(c), "G91\nG0 %c%.3f F%d\nG90", names[axis], dist, axis == 2 ? 600 : 3000); gcode(c);
}
void printer_home(int axis)
{
    static const char *cmd[] = {"G28 X", "G28 Y", "G28 Z", "G28 X Y"};
    gcode(axis < 0 ? "G28" : (axis <= 3 ? cmd[axis] : "G28"));
}
void printer_motors_off(void) { gcode("M84"); }
void printer_extrude(float mm) { char c[64]; snprintf(c, sizeof(c), "M83\nG1 E%.2f F300", mm); gcode(c); }
void printer_print_start(const char *filename)
{
    if (!filename) return;
    char escaped[192];
    size_t n = 0;
    for (const char *s = filename; *s && n + 2 < sizeof(escaped); ++s) {
        if (*s == '"' || *s == '\\') escaped[n++] = '\\';
        escaped[n++] = *s;
    }
    escaped[n] = 0;
    char param[224];
    snprintf(param, sizeof(param), "{\"filename\":\"%s\"}", escaped);
    (void)send_rpc_text("printer.print.start", param);
}
void printer_print_pause(void) { (void)send_rpc_text("printer.print.pause", nullptr); }
void printer_print_resume(void) { (void)send_rpc_text("printer.print.resume", nullptr); }
void printer_print_cancel(void) { (void)send_rpc_text("printer.print.cancel", nullptr); }
void printer_emergency_stop(void) { (void)send_rpc_text("printer.emergency_stop", nullptr); }
void printer_firmware_restart(void) { gcode("FIRMWARE_RESTART"); }

bool printer_files_refresh(unsigned offset, printer_files_cb cb, void *ud)
{
    if (!cb || mr_state != MOONRAKER_READY || file_req.active) return false;
    ++file_req.generation;
    file_req.id = next_id;
    file_req.offset = offset;
    file_req.started = millis();
    file_req.cb = cb;
    file_req.ud = ud;
    memset(&file_req.page, 0, sizeof(file_req.page));
    file_req.ready = false;
    file_req.active = true;
    __DMB();
    if (!queue_rpc_text("server.files.list", "{\"root\":\"gcodes\"}")) {
        file_req.active = false;
        file_req.cb = nullptr;
        return false;
    }
    return true;
}
void printer_files_poll(void)
{
    if (file_req.active && millis() - file_req.started > 8000) {
        file_req.page.count = -1;
        file_req.result_generation = file_req.generation;
        __DMB();
        file_req.ready = true;
        file_req.active = false;
    }
    if (!file_req.ready) return;
    __DMB();
    printer_files_cb cb = file_req.cb;
    void *ud = file_req.ud;
    printer_file_page_t page = file_req.page;
    bool current = file_req.result_generation == file_req.generation;
    file_req.ready = false;
    file_req.cb = nullptr;
    if (cb && current) cb(&page, ud);
}
void printer_files_cancel(void)
{
    ++file_req.generation;
    file_req.active = false;
    file_req.ready = false;
    file_req.cb = nullptr;
}
void printer_file_delete(const char *name)
{
    if (!name || !name[0]) return;
    char escaped[208];
    size_t n = 0;
    const char prefix[] = "gcodes/";
    memcpy(escaped, prefix, sizeof(prefix) - 1);
    n = sizeof(prefix) - 1;
    for (const char *s = name; *s && n + 2 < sizeof(escaped); ++s) {
        if (*s == '"' || *s == '\\') escaped[n++] = '\\';
        escaped[n++] = *s;
    }
    escaped[n] = 0;
    char param[240];
    snprintf(param, sizeof(param), "{\"path\":\"%s\"}", escaped);
    (void)send_rpc_text("server.files.delete", param);
}
int printer_macro_count(void) { return 0; }
const char *printer_macro_name(int) { return ""; }
const char *printer_macro_label(int) { return ""; }
void printer_macro_run(int) {}
bool printer_macro_params_loading(void) { return false; }
int printer_macro_param_count(int) { return 0; }
bool printer_macro_param_info(int, int, char *, size_t, char *, size_t, bool *) { return false; }
void printer_macro_run_with(int, const char *const *) {}
int printer_fan_count(void) { return 0; }
const char *printer_fan_name(int) { return ""; }
float printer_fan_speed(int) { return -1; }
bool printer_fan_writable(int) { return false; }
void printer_fan_set(int, float) {}
int printer_filsensor_count(void) { return 0; }
const char *printer_filsensor_name(int) { return ""; }
bool printer_filsensor_detected(int) { return false; }
bool printer_filsensor_enabled(int) { return false; }
void printer_filsensor_set_enabled(int, bool) {}
void printer_endstop_refresh(void) {}
int printer_endstop_state(int) { return -1; }
uint32_t printer_endstop_age_ms(void) { return UINT32_MAX; }
void printer_console_send(const char *cmd) { gcode(cmd); }
int printer_console_line_count(void) { return 0; }
const char *printer_console_line(int, int *kind) { if (kind) *kind = 0; return ""; }
void printer_console_clear(void) {}
void printer_console_load_history(void) {}
int printer_zcal_command_count(void) { return 0; }
const char *printer_zcal_command(int) { return ""; }
bool printer_zcal_commands_pending(void) { return false; }
bool printer_probe_present(void) { return false; }
bool printer_probe_z_offset(float *) { return false; }
bool printer_homing_origin_z(float *) { return false; }
bool printer_zcal_active(void) { return false; }
void printer_zcal_start(const char *) {}
void printer_zcal_testz(float) {}
void printer_zcal_accept(void) {}
void printer_zcal_abort(void) {}

void moonraker_start(void)
{
    disabled = false;
    mr_state = MOONRAKER_OFFLINE;
    p.state = PRINTER_STATE_DISCONNECTED;
    ++network_epoch;
}
void moonraker_stop(void)
{
    disabled = true;
    mr_state = MOONRAKER_OFFLINE;
    p.state = PRINTER_STATE_DISCONNECTED;
    ++network_epoch;
}
void moonraker_reload(void)
{
    disabled = false;
    mr_state = MOONRAKER_OFFLINE;
    p.state = PRINTER_STATE_DISCONNECTED;
    ++network_epoch;
}
moonraker_state_t moonraker_state(void) { return mr_state; }
int moonraker_status_age_s(void) { return -1; }
bool moonraker_status_gate_pending(void) { return false; }
bool moonraker_send_rpc(const char *method, const char *params_json)
{
    return send_rpc_text(method, params_json);
}
bool moonraker_rpc(const char *, const char *, void (*)(char *, void *), void *)
{
    return false; /* bounded fire-and-forget backend; file callbacks are disabled */
}

void printer_wio_debug_status(void)
{
    Serial.print("Moonraker: state=");
    Serial.print((int)mr_state);
    Serial.print(" link=");
    Serial.print((int)link_state);
    Serial.print(" printer=");
    Serial.print((int)p.state);
    Serial.print(" tx=");
    Serial.print((unsigned long)tx_message_count);
    Serial.print(" rx=");
    Serial.print((unsigned long)rx_message_count);
    Serial.print(" last_rx=");
    Serial.print((unsigned long)last_rx_message_len);
    Serial.print(" overflow=");
    Serial.println(last_rx_overflow ? 1 : 0);
}

} // extern "C"
