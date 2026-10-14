#include "gcode_thumbnail.h"
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 缩略图注释块一般在文件头（PrusaSlicer/Orca/Cura）；个别切片器放尾部。 */
#define SCAN_CHUNK (512 * 1024)

bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap)
{
    if (!gcode_name || !gcode_name[0]) return false;
    const char *dir = getenv("KLIPPER_GCODES_DIR");
    char fallback[512];
    if (!dir || !dir[0]) {
        const char *home = getenv("HOME");
        if (!home) return false;
        snprintf(fallback, sizeof(fallback), "%s/printer_data/gcodes", home);
        dir = fallback;
    }
    int n = snprintf(out, cap, "%s/%s", dir, gcode_name);
    return n > 0 && (size_t)n < cap;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 就地解码 base64 段（已去掉 "; " 前缀与空白），返回解码后长度（<=srclen）。 */
static size_t b64_decode(const char *src, size_t srclen, unsigned char *dst)
{
    size_t out = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < srclen; i++) {
        if (src[i] == '=') break;
        int v = b64val((unsigned char)src[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

/* 在一块文本缓冲里找最大缩略图；命中返回 malloc 的 PNG（调用方 free）。 */
static bool scan_buf(const char *buf, size_t len, unsigned char **png_out, size_t *png_len)
{
    const char *p = buf, *end = buf + len;
    unsigned char *best = NULL;
    size_t best_len = 0;
    long best_area = 0;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        size_t ll = (size_t)(line_end - p);

        int w = 0, h = 0;
        if (ll > 18 && memcmp(p, "; thumbnail begin", 17) == 0 &&
            sscanf(p + 17, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            /* 收集 base64 行直到 "; thumbnail end"；base64 行以 "; " 开头 */
            size_t b64_cap = (size_t)w * h * 3 + 1024;   /* PNG 通常远小于 raw RGB */
            char *b64 = malloc(b64_cap);
            size_t b64_len = 0;
            const char *q = line_end < end ? line_end + 1 : end;
            bool closed = false;
            while (b64 && q < end) {
                const char *qe = memchr(q, '\n', (size_t)(end - q));
                if (!qe) qe = end;
                size_t ql = (size_t)(qe - q);
                if (ql >= 15 && memcmp(q, "; thumbnail end", 15) == 0) { closed = true; break; }
                if (ql > 2 && q[0] == ';' && q[1] == ' ') {
                    size_t payload = ql - 2;
                    if (payload > 0 && (q[2] == ';' || payload > 1024 * 10)) break;  /* 注释跑偏 */
                    if (b64_len + payload + 1 > b64_cap) {
                        b64_cap = (b64_len + payload + 1) * 2;
                        char *nb = realloc(b64, b64_cap);
                        if (!nb) { free(b64); b64 = NULL; break; }
                        b64 = nb;
                    }
                    memcpy(b64 + b64_len, q + 2, payload);
                    b64_len += payload;
                } else break;   /* 非缩略图行：块损坏，放弃本块 */
                q = qe < end ? qe + 1 : end;
            }
            if (b64 && closed && b64_len > 8) {
                unsigned char *png = malloc(b64_len * 3 / 4 + 4);
                if (png) {
                    size_t plen = b64_decode(b64, b64_len, png);
                    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
                    if (plen > sizeof(magic) && memcmp(png, magic, sizeof(magic)) == 0) {
                        long area = (long)w * h;
                        if (area > best_area) {
                            free(best);
                            best = png; best_len = plen; best_area = area;
                        } else free(png);
                    } else free(png);
                }
            }
            free(b64);
            p = q;
            continue;
        }
        p = line_end < end ? line_end + 1 : end;
    }

    if (!best) return false;
    *png_out = best;
    *png_len = best_len;
    return true;
}

bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len)
{
    FILE *f = fopen(gcode_path, "rb");
    if (!f) return false;
    char *buf = malloc(SCAN_CHUNK);
    if (!buf) { fclose(f); return false; }

    bool ok = false;
    size_t n = fread(buf, 1, SCAN_CHUNK, f);
    if (n > 0) ok = scan_buf(buf, n, png_out, png_len);
    if (!ok) {
        /* 头部没找到：扫尾部一段（SuperSlicer 等放在文件尾） */
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            long off = sz > SCAN_CHUNK ? sz - SCAN_CHUNK : 0;
            if (off > 0 && fseek(f, off, SEEK_SET) == 0) {
                n = fread(buf, 1, SCAN_CHUNK, f);
                if (n > 0) ok = scan_buf(buf, n, png_out, png_len);
            }
        }
    }
    free(buf);
    fclose(f);
    return ok;
}

#else /* !BSP_HAS_LINUX_HOST */

bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap)
{
    (void)gcode_name; (void)out; (void)cap;
    return false;
}

bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len)
{
    (void)gcode_path; (void)png_out; (void)png_len;
    return false;
}

#endif

/* ---------- 远程拉取：Moonraker HTTP ----------
 * ESP32（带 PSRAM）与 Windows 桌面端的唯一路径；Linux 上位机连远程主机、本地
 * gcodes 目录没有该文件时的回退路径。流程与 KlipperScreen 相同：先 metadata 拿
 * Moonraker 已提取的缩略图 PNG 相对路径，再下载该小图。 */
#if BSP_HAS_GCODE_THUMB

#include "app_settings.h"
#include "bsp.h"
#include "cJSON.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define THUMB_LOG(...) ESP_LOGD("gcode_thumb", __VA_ARGS__)
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define THUMB_LOG(...) ((void)0)
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#define THUMB_LOG(...) ((void)0)
#endif

#define THUMB_META_CAP  (64 * 1024)    /* metadata JSON 上限 */
#define THUMB_PNG_CAP   (1024 * 1024)  /* 缩略图 PNG 上限（正常 <200KB） */

typedef struct {
    void (*cb)(unsigned char *png, size_t len, void *ud);
    void *ud;
    unsigned char *png;
    size_t png_len;
    char name[96];
} thumb_job_t;

/* URL 路径/查询百分号编码：保留 unreserved + '/'（相对路径含子目录分隔） */
static void url_encode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* 同步 GET，返回 malloc 的响应体（NUL 结尾；ESP32 上 >16KB 自动落 PSRAM），失败 NULL。 */
#ifdef ESP_PLATFORM
static char *http_get(const char *host, uint16_t port, const char *path,
                      size_t cap, size_t *out_len)
{
    char url[512];
    snprintf(url, sizeof(url), "http://%s:%u%s", host, (unsigned)port, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    char *body = NULL;
    size_t used = 0;
    if (esp_http_client_open(client, 0) != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(client) < 0) goto done;
    if (esp_http_client_get_status_code(client) != 200) goto done;

    body = malloc(cap + 1);
    if (!body) goto done;
    while (used < cap) {
        int got = esp_http_client_read(client, body + used, cap - used);
        if (got < 0) goto done;
        if (got == 0) break;
        used += (size_t)got;
    }
    body[used] = 0;
done:
    esp_http_client_cleanup(client);
    if (!body) return NULL;
    if (out_len) *out_len = used;
    return body;
}
#else /* Linux 上位机：裸 POSIX socket，Connection: close 读到 EOF */
#ifndef _WIN32
static char *http_get(const char *host, uint16_t port, const char *path,
                      size_t cap, size_t *out_len)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return NULL;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { 4, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    char req[600];
    int rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
                      path, host, (unsigned)port);
    if (rn <= 0 || send(fd, req, (size_t)rn, 0) != rn) { close(fd); return NULL; }

    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return NULL; }
    size_t used = 0;
    while (used < cap) {
        ssize_t got = recv(fd, buf + used, cap - used, 0);
        if (got <= 0) break;   /* EOF / 超时：以已收到的为准，交给校验 */
        used += (size_t)got;
    }
    close(fd);
    buf[used] = 0;

    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 &&
        strncmp(buf, "HTTP/1.0 200", 12) != 0) {
        free(buf);
        return NULL;
    }
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) { free(buf); return NULL; }
    size_t hdr = (size_t)(sep + 4 - buf);
    size_t body_len = used > hdr ? used - hdr : 0;
    memmove(buf, buf + hdr, body_len + 1);
    if (out_len) *out_len = body_len;
    return buf;
}
#else /* Windows：Winsock2，语义同上（Connection: close 读到 EOF） */
static char *http_get(const char *host, uint16_t port, const char *path,
                      size_t cap, size_t *out_len)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;   /* 引用计数，无需配对 cleanup */

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return NULL;
    SOCKET fd = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        DWORD tv = 4000;   /* Winsock 的 RCVTIMEO/SNDTIMEO 是毫秒 DWORD */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(fd);
        fd = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) return NULL;

    char req[600];
    int rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
                      path, host, (unsigned)port);
    if (rn <= 0 || send(fd, req, rn, 0) != rn) { closesocket(fd); return NULL; }

    char *buf = malloc(cap + 1);
    if (!buf) { closesocket(fd); return NULL; }
    size_t used = 0;
    while (used < cap) {
        int got = recv(fd, buf + used, (int)(cap - used), 0);
        if (got <= 0) break;   /* EOF / 超时：以已收到的为准，交给校验 */
        used += (size_t)got;
    }
    closesocket(fd);
    buf[used] = 0;

    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 &&
        strncmp(buf, "HTTP/1.0 200", 12) != 0) {
        free(buf);
        return NULL;
    }
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) { free(buf); return NULL; }
    size_t hdr = (size_t)(sep + 4 - buf);
    size_t body_len = used > hdr ? used - hdr : 0;
    memmove(buf, buf + hdr, body_len + 1);
    if (out_len) *out_len = body_len;
    return buf;
}
#endif
#endif

/* metadata JSON → 最大缩略图的相对路径（目录名由 Moonraker 返回）。 */
static bool pick_thumb_path(const char *json, char *out, size_t cap)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *thumbs = result ? cJSON_GetObjectItemCaseSensitive(result, "thumbnails") : NULL;
    bool ok = false;
    if (cJSON_IsArray(thumbs)) {
        long best_area = 0;
        const char *best = NULL;
        cJSON *it;
        cJSON_ArrayForEach(it, thumbs) {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "width");
            cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "height");
            cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "relative_path");
            if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h) || !cJSON_IsString(p)) continue;
            long area = (long)w->valuedouble * (long)h->valuedouble;
            if (area > best_area && p->valuestring[0]) {
                best_area = area;
                best = p->valuestring;
            }
        }
        if (best) {
            int n = snprintf(out, cap, "%s", best);
            ok = n > 0 && (size_t)n < cap;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* lv_async_call 投递：保证 cb 恰好一次且总在 LVGL 上下文。
 * png 所有权随 cb 调用移交（cb 负责 free 或接管），这里只释放 job 本体。 */
static void thumb_deliver(void *arg)
{
    thumb_job_t *job = arg;
    job->cb(job->png, job->png_len, job->ud);
    free(job);
}

/* lv_async_call() 本身不是跨线程队列：它会直接修改 LVGL 的 timer 链表。
 * 下载 worker 与 lv_timer_handler 并行时必须走项目统一的 LVGL 锁，否则会
 * 随机破坏链表，表现为 SIGSEGV 或 lv_free 断言后的 SIGABRT。 */
static void post_thumb_delivery(thumb_job_t *job)
{
    bsp_lvgl_lock();
    lv_result_t result = lv_async_call(thumb_deliver, job);
    if (result != LV_RESULT_OK) {
        /* 任务已经向调用方承诺恰好一次回调。timer 分配失败时在持锁的
         * worker 上同步投递失败结果，让 widget 能结束其后到者所有权状态。 */
        free(job->png);
        job->png = NULL;
        job->png_len = 0;
        thumb_deliver(job);
    }
    bsp_lvgl_unlock();
}

static void thumb_fetch_run(thumb_job_t *job)
{
    moonraker_conf_t conf;
    char path[400], enc[288];

    if (!settings_load_moonraker(&conf) || !conf.valid || !conf.host[0]) return;
    uint16_t port = conf.port ? conf.port : 7125;

    url_encode(job->name, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/server/files/metadata?filename=%s", enc);
    size_t meta_len = 0;
    char *meta = http_get(conf.host, port, path, THUMB_META_CAP, &meta_len);
    if (!meta) return;

    char rel[160];
    bool have = pick_thumb_path(meta, rel, sizeof(rel));
    free(meta);
    if (!have) return;

    url_encode(rel, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/server/files/gcodes/%s", enc);
    size_t png_len = 0;
    unsigned char *png = (unsigned char *)http_get(conf.host, port, path,
                                                   THUMB_PNG_CAP, &png_len);
    if (png && png_len > 8 && memcmp(png, "\x89PNG\x0d\x0a\x1a\x0a", 8) == 0) {
        job->png = png;
        job->png_len = png_len;
    } else {
        free(png);
    }
}

#ifdef ESP_PLATFORM
static void thumb_fetch_task(void *arg)
{
    thumb_job_t *job = arg;
    thumb_fetch_run(job);
    if (!job->png) THUMB_LOG("no thumbnail for %s", job->name);
    post_thumb_delivery(job);
    vTaskDelete(NULL);
}
#elif defined(_WIN32)
static DWORD WINAPI thumb_fetch_thread(LPVOID arg)
{
    thumb_job_t *job = arg;
    thumb_fetch_run(job);
    if (!job->png) THUMB_LOG("no thumbnail for %s", job->name);
    post_thumb_delivery(job);
    return 0;
}
#else
static void *thumb_fetch_thread(void *arg)
{
    thumb_job_t *job = arg;
    thumb_fetch_run(job);
    if (!job->png) THUMB_LOG("no thumbnail for %s", job->name);
    post_thumb_delivery(job);
    return NULL;
}
#endif

bool gcode_thumbnail_fetch_async(const char *gcode_name,
                                 void (*cb)(unsigned char *png, size_t len, void *ud),
                                 void *ud)
{
    if (!gcode_name || !gcode_name[0] || !cb) return false;
    thumb_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->cb = cb;
    job->ud = ud;
    snprintf(job->name, sizeof(job->name), "%s", gcode_name);
#ifdef ESP_PLATFORM
    /* 6KB 栈走内部 RAM；esp_http_client + cJSON 实测 4KB 不够富余 */
    if (xTaskCreate(thumb_fetch_task, "gthumb", 6144, job, 4, NULL) != pdPASS) {
        free(job);
        return false;
    }
#elif defined(_WIN32)
    /* CreateThread 免 pthread 依赖（MinGW 的 winpthread 会多拖一个 DLL） */
    HANDLE th = CreateThread(NULL, 0, thumb_fetch_thread, job, 0, NULL);
    if (!th) {
        free(job);
        return false;
    }
    CloseHandle(th);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, thumb_fetch_thread, job) != 0) {
        free(job);
        return false;
    }
    pthread_detach(th);
#endif
    return true;
}

#else

bool gcode_thumbnail_fetch_async(const char *gcode_name,
                                 void (*cb)(unsigned char *png, size_t len, void *ud),
                                 void *ud)
{
    (void)gcode_name; (void)cb; (void)ud;
    return false;
}

#endif
