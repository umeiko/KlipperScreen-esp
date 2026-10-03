/*
 * Bambu Cloud 账号登录（POSIX 后端：Linux 桌面 / Android）。
 * 与 Windows 的 bambu_cloud_winhttp.c 同一状态机与云接口：
 * 密码/邮箱验证码/短信验证码/TFA 登录，设备列表，JWT/Profile 解析 user_id，
 * 令牌持久化。差异：HTTPS 传输用 bambu_tls（mbedTLS）+ 手写的最小 HTTP/1.1
 * 客户端（Content-Length / chunked / close 三种正文定界，Set-Cookie 收集）；
 * 令牌在 POSIX 上没有 DPAPI 可用，按明文三行格式存 bsp_conf（Linux 配置目录
 * 0700 / Android 应用私有目录保护；与 Windows 的 DPAPI 密文格式互不通用）。
 */
#include "bambu_cloud.h"
#include "bambu_cloud_internal.h"
#include "bambu_tls.h"
#include "bsp_conf.h"
#include "cJSON.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_MAX       2048
#define TFA_KEY_MAX      512
#define HTTP_BODY_MAX (256 * 1024)
#define HTTP_HEAD_MAX    16384
#define HTTP_READ_IDLE_MAX 10   /* 每次读允许的空转秒数（SO_RCVTIMEO=1s） */

typedef struct {
    int status;
    char *body;
    char cookies[4096];
    int net_error;
} http_result_t;

typedef enum {
    OP_PASSWORD,
    OP_REQUEST_EMAIL_CODE,
    OP_REQUEST_SMS_CODE,
    OP_SUBMIT_CODE,
    OP_REFRESH,
} worker_op_t;

typedef struct {
    worker_op_t op;
    int generation;
    bambu_cloud_region_t region;
    bambu_cloud_state_t challenge;
    char account[BAMBU_CLOUD_ACCOUNT_MAX];
    char password[192];
    char code[32];
    char token[TOKEN_MAX];
    char tfa_key[TFA_KEY_MAX];
} worker_args_t;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock;
static bambu_cloud_snapshot_t g_snapshot;
static char g_token[TOKEN_MAX];
static char g_user_id[96];
static char g_tfa_key[TFA_KEY_MAX];
static atomic_int g_busy;
static atomic_int g_generation = 1;

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void wipe(void *p, size_t n) { memset(p, 0, n); }

static const char *api_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? "api.bambulab.cn" : "api.bambulab.com";
}

static const char *site_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? "bambulab.cn" : "bambulab.com";
}

/* ---------------- cookie 罐（与 winhttp 版同一格式："a=1; b=2"） ---------------- */

static void cookie_put(char *jar, size_t cap, const char *set_cookie)
{
    const char *semi = strchr(set_cookie, ';');
    size_t pair_len = semi ? (size_t)(semi - set_cookie) : strlen(set_cookie);
    const char *eq = memchr(set_cookie, '=', pair_len);
    if (!eq || eq == set_cookie || eq + 1 >= set_cookie + pair_len) return;
    size_t name_len = (size_t)(eq - set_cookie) + 1;

    char name[128];
    if (name_len >= sizeof(name)) return;
    memcpy(name, set_cookie, name_len);
    name[name_len] = 0;

    char *at = jar;
    while ((at = strstr(at, name)) != NULL) {
        if ((at == jar || (at >= jar + 2 && at[-2] == ';' && at[-1] == ' ')) &&
            strncmp(at, set_cookie, name_len) == 0) {
            char *end = strchr(at, ';');
            if (end) end += end[1] == ' ' ? 2 : 1;
            else end = at + strlen(at);
            memmove(at, end, strlen(end) + 1);
            break;
        }
        at++;
    }
    size_t used = strlen(jar);
    size_t extra = pair_len + (used ? 2 : 0);
    if (used + extra + 1 > cap) return;
    if (used) strcat(jar, "; ");
    strncat(jar, set_cookie, pair_len);
}

static bool cookie_value(const char *jar, const char *name, char *out, size_t cap)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "%s=", name);
    size_t nlen = strlen(needle);
    const char *p = jar;
    while ((p = strstr(p, needle)) != NULL) {
        if (p == jar || (p >= jar + 2 && p[-2] == ';' && p[-1] == ' ')) {
            const char *v = p + nlen;
            const char *end = strchr(v, ';');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return len != 0;
        }
        p++;
    }
    if (cap) out[0] = 0;
    return false;
}

/* ---------------- 最小 HTTP/1.1 客户端（bambu_tls 之上） ---------------- */

/* 读一个字节：1=成功 0=超时 -1=断开/错误 */
static int http_read_byte(bambu_tls_t *tls, char *out)
{
    return bambu_tls_read(tls, out, 1);
}

/* 读满 len：0=成功 -1=失败；允许 HTTP_READ_IDLE_MAX 秒空转 */
static int http_read_exact(bambu_tls_t *tls, char *out, size_t len)
{
    size_t done = 0;
    int idle = 0;
    while (done < len) {
        int n = bambu_tls_read(tls, out + done, (int)(len - done));
        if (n > 0) { done += (size_t)n; idle = 0; continue; }
        if (n == 0 && ++idle <= HTTP_READ_IDLE_MAX) continue;
        return -1;
    }
    return 0;
}

/* 读一行（不含 \r\n）：返回长度，-1=失败（含超时预算耗尽） */
static int http_read_line(bambu_tls_t *tls, char *out, size_t cap)
{
    size_t used = 0;
    char prev = 0;
    int idle = 0;
    for (;;) {
        char c;
        int n = http_read_byte(tls, &c);
        if (n < 0) return -1;
        if (n == 0) {
            if (++idle > HTTP_READ_IDLE_MAX) return -1;   /* 超时累计上限，防死等 */
            continue;
        }
        idle = 0;
        if (c == '\n' && prev == '\r') {
            if (used) used--;   /* 去掉 \r */
            break;
        }
        if (used + 1 < cap) out[used++] = c;
        prev = c;
        if (used + 1 >= HTTP_HEAD_MAX) return -1;
    }
    out[used] = 0;
    return (int)used;
}

static int header_append_body(char **body, size_t *used, size_t *cap,
                              const char *data, size_t n)
{
    if (*used + n + 1 > HTTP_BODY_MAX) return -1;
    if (*used + n + 1 > *cap) {
        size_t next = *cap;
        while (next < *used + n + 1) next *= 2;
        char *grown = realloc(*body, next);
        if (!grown) return -1;
        *body = grown;
        *cap = next;
    }
    memcpy(*body + *used, data, n);
    *used += n;
    return 0;
}

static bool http_request(const char *host, const char *path, const char *method,
                         const char *json, const char *bearer,
                         const char *cookie_jar, const char *csrf,
                         http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->body = malloc(4096);
    size_t body_cap = 4096, body_len = 0;
    if (!out->body) return false;
    out->body[0] = 0;

    bambu_tls_t *tls = bambu_tls_connect(host, "443", 6000);
    if (!tls) { out->net_error = 1; return false; }

    size_t json_len = json ? strlen(json) : 0;
    char head[8192];
    int hn = snprintf(head, sizeof(head),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: bambu_network_agent/01.09.05.01\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n",
        method, path, host, json_len);
    if (bearer && bearer[0])
        hn += snprintf(head + hn, sizeof(head) - hn, "Authorization: Bearer %s\r\n", bearer);
    if (cookie_jar && cookie_jar[0])
        hn += snprintf(head + hn, sizeof(head) - hn, "Cookie: %s\r\n", cookie_jar);
    if (csrf && csrf[0])
        hn += snprintf(head + hn, sizeof(head) - hn, "x-bbl-csrf-token: %s\r\n", csrf);
    hn += snprintf(head + hn, sizeof(head) - hn, "\r\n");

    bool ok = hn > 0 && hn < (int)sizeof(head) &&
              bambu_tls_write_all(tls, head, hn) == 0 &&
              (!json_len || bambu_tls_write_all(tls, json, (int)json_len) == 0);
    if (!ok) { out->net_error = 1; bambu_tls_close(tls); return false; }

    /* 状态行 */
    char line[2048];
    if (http_read_line(tls, line, sizeof(line)) < 0) {
        out->net_error = 1; bambu_tls_close(tls); return false;
    }
    if (sscanf(line, "HTTP/%*s %d", &out->status) != 1) {
        out->net_error = 1; bambu_tls_close(tls); return false;
    }

    /* 头部 */
    long content_length = -1;
    bool chunked = false;
    if (cookie_jar) copy_text(out->cookies, sizeof(out->cookies), cookie_jar);
    for (;;) {
        if (http_read_line(tls, line, sizeof(line)) < 0) {
            out->net_error = 1; bambu_tls_close(tls); return false;
        }
        if (!line[0]) break;   /* 空行 = 头结束 */
        if (strncasecmp(line, "Set-Cookie:", 11) == 0) {
            const char *v = line + 11;
            while (*v == ' ' || *v == '\t') v++;
            cookie_put(out->cookies, sizeof(out->cookies), v);
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = strtol(line + 15, NULL, 10);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strstr(line, "chunked")) chunked = true;
        }
    }

    /* 正文：chunked / Content-Length / 读到关闭 */
    bool fail = false;
    if (chunked) {
        for (;;) {
            if (http_read_line(tls, line, sizeof(line)) < 0) { fail = true; break; }
            long chunk = strtol(line, NULL, 16);
            if (chunk < 0) { fail = true; break; }
            if (chunk == 0) break;   /* 尾块（trailer 忽略，直接读到关闭即可） */
            char buf[8192];
            while (chunk > 0) {
                size_t want = (size_t)chunk < sizeof(buf) ? (size_t)chunk : sizeof(buf);
                if (http_read_exact(tls, buf, want) != 0) { fail = true; break; }
                if (header_append_body(&out->body, &body_len, &body_cap, buf, want) != 0) {
                    fail = true; break;
                }
                chunk -= (long)want;
            }
            if (fail) break;
            char crlf[2];
            if (http_read_exact(tls, crlf, 2) != 0) { fail = true; break; }
        }
    } else if (content_length >= 0) {
        char buf[8192];
        long left = content_length;
        while (left > 0 && !fail) {
            size_t want = (size_t)left < sizeof(buf) ? (size_t)left : sizeof(buf);
            if (http_read_exact(tls, buf, want) != 0) { fail = true; break; }
            if (header_append_body(&out->body, &body_len, &body_cap, buf, want) != 0) {
                fail = true; break;
            }
            left -= (long)want;
        }
    } else {
        /* Connection: close → 读到断（空转预算用完后等对端关） */
        char buf[8192];
        int idle = 0;
        for (;;) {
            int n = bambu_tls_read(tls, buf, sizeof(buf));
            if (n > 0) {
                if (header_append_body(&out->body, &body_len, &body_cap, buf,
                                       (size_t)n) != 0) { fail = true; break; }
                continue;
            }
            if (n == 0 && ++idle <= HTTP_READ_IDLE_MAX) continue;
            break;   /* -1 关闭或超时耗尽：Content-Length 未知时只能到此 */
        }
    }
    bambu_tls_close(tls);

    out->body[body_len] = 0;
    if (fail) { out->net_error = 1; return false; }
    return true;
}

static void http_free(http_result_t *r)
{
    free(r->body);
    r->body = NULL;
}

/* ---------------- JSON 工具 ---------------- */

static cJSON *json_data(cJSON *root)
{
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsObject(data) ? data : root;
}

static const char *json_string(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static const char *extract_token(cJSON *root)
{
    const char *v = json_string(root, "accessToken");
    if (!v) v = json_string(root, "token");
    if (!v) {
        cJSON *data = json_data(root);
        v = json_string(data, "accessToken");
        if (!v) v = json_string(data, "token");
    }
    return v;
}

static bool copy_uid_value(cJSON *obj, char *out, size_t cap)
{
    if (!cJSON_IsObject(obj) || !out || cap < 4) return false;
    static const char *keys[] = {"uidStr", "uid", "sub", "userId", "user_id"};
    char raw[80] = {0};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
            copy_text(raw, sizeof(raw), v->valuestring);
            break;
        }
        if (cJSON_IsNumber(v)) {
            snprintf(raw, sizeof(raw), "%.0f", v->valuedouble);
            break;
        }
    }
    if (!raw[0]) return false;
    if (strncmp(raw, "u_", 2) == 0) copy_text(out, cap, raw);
    else snprintf(out, cap, "u_%s", raw);
    wipe(raw, sizeof(raw));
    return out[0] != 0;
}

/* ---------------- base64（JWT payload 解析用） ---------------- */

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const char *in, size_t len, size_t *out_len)
{
    if (len % 4 != 0) return NULL;
    size_t cap = len / 4 * 3 + 1;
    unsigned char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=') { v[j] = 0; pad++; }
            else if ((v[j] = b64val(c)) < 0) { free(out); return NULL; }
        }
        unsigned triple = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) |
                          ((unsigned)v[2] << 6) | (unsigned)v[3];
        /* 输出 3-pad 字节：pad=0→3、pad=1→2、pad=2→1 */
        out[n++] = (unsigned char)(triple >> 16);
        if (pad < 2) out[n++] = (unsigned char)(triple >> 8);
        if (pad < 1) out[n++] = (unsigned char)triple;
    }
    out[n] = 0;   /* 调用方常按 C 字符串解析（cJSON），补终止符 */
    *out_len = n;
    return out;
}

static bool jwt_user_id(const char *token, char *out, size_t cap)
{
    const char *dot1 = token ? strchr(token, '.') : NULL;
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2 || dot2 <= dot1 + 1) return false;
    size_t src_len = (size_t)(dot2 - dot1 - 1);
    size_t padded_len = (src_len + 3u) & ~3u;
    char *encoded = calloc(padded_len + 1, 1);
    if (!encoded) return false;
    memcpy(encoded, dot1 + 1, src_len);
    for (size_t i = 0; i < src_len; i++) {
        if (encoded[i] == '-') encoded[i] = '+';
        else if (encoded[i] == '_') encoded[i] = '/';
    }
    for (size_t i = src_len; i < padded_len; i++) encoded[i] = '=';

    size_t decoded_len = 0;
    unsigned char *decoded = base64_decode(encoded, padded_len, &decoded_len);
    wipe(encoded, padded_len);
    free(encoded);
    if (!decoded) return false;
    cJSON *payload = cJSON_ParseWithLength((const char *)decoded, decoded_len);
    wipe(decoded, decoded_len + 1);
    free(decoded);
    bool found = copy_uid_value(payload, out, cap);
    cJSON_Delete(payload);
    return found;
}

static bool resolve_user_id(bambu_cloud_region_t region, const char *token,
                            char *out, size_t cap)
{
    if (jwt_user_id(token, out, cap)) return true;
    http_result_t r = {0};
    bool ok = http_request(api_host(region), "/v1/user-service/my/profile",
                           "GET", NULL, token, NULL, NULL, &r);
    if (!ok || r.status != 200) { http_free(&r); return false; }
    cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
    cJSON *data = root ? json_data(root) : NULL;
    bool found = copy_uid_value(root, out, cap) ||
                 (data != root && copy_uid_value(data, out, cap));
    cJSON_Delete(root);
    http_free(&r);
    return found;
}

static void response_message(http_result_t *r, const char *fallback,
                             char *out, size_t cap)
{
    const char *found = NULL;
    cJSON *root = r->body ? cJSON_Parse(r->body) : NULL;
    if (root) {
        found = json_string(root, "error");
        if (!found) found = json_string(root, "message");
        cJSON *data = json_data(root);
        if (!found && data != root) found = json_string(data, "message");
    }
    if (found && found[0]) copy_text(out, cap, found);
    else if (r->status) snprintf(out, cap, "%s（HTTP %d）", fallback, r->status);
    else copy_text(out, cap, fallback);
    cJSON_Delete(root);
}

/* ---------------- 令牌持久化（明文三行：region / account / token） ---------------- */

static bool secret_save(const char *account, const char *token,
                        bambu_cloud_region_t region)
{
    size_t plain_len = strlen(account) + strlen(token) + 8;
    char *plain = malloc(plain_len);
    if (!plain) return false;
    snprintf(plain, plain_len, "%d\n%s\n%s", (int)region, account, token);
    int rc = bsp_conf_write("bambu_cloud.secret", plain);
    wipe(plain, plain_len);
    free(plain);
    return rc == 0;
}

static bool secret_load(char *account, size_t account_cap, char *token,
                        size_t token_cap, bambu_cloud_region_t *region)
{
    char *text = malloc(8192);
    if (!text) return false;
    int n = bsp_conf_read("bambu_cloud.secret", text, 8192);
    if (n <= 1) { free(text); return false; }
    text[n] = 0;
    char *line1 = text, *line2 = strchr(line1, '\n');
    char *line3 = line2 ? strchr(line2 + 1, '\n') : NULL;
    bool ok = false;
    if (line2 && line3) {
        *line2 = 0; *line3 = 0;
        *region = atoi(line1) == BAMBU_CLOUD_REGION_CHINA
                ? BAMBU_CLOUD_REGION_CHINA : BAMBU_CLOUD_REGION_GLOBAL;
        copy_text(account, account_cap, line2 + 1);
        copy_text(token, token_cap, line3 + 1);
        ok = account[0] && token[0];
    }
    wipe(text, 8192);
    free(text);
    return ok;
}

/* ---------------- 状态发布 ---------------- */

static void once_init(void)
{
    pthread_mutex_init(&g_lock, NULL);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_OUT;
    g_snapshot.region = BAMBU_CLOUD_REGION_CHINA;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "请输入拓竹账号");
    if (secret_load(g_snapshot.account, sizeof(g_snapshot.account), g_token,
                     sizeof(g_token), &g_snapshot.region)) {
        jwt_user_id(g_token, g_user_id, sizeof(g_user_id));
        g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), "已恢复保存的登录");
    }
}

void bambu_cloud_init(void)
{
    pthread_once(&g_once, once_init);
}

static bool generation_alive(int generation)
{
    return atomic_load(&g_generation) == generation;
}

static void publish_state(int generation, bambu_cloud_state_t state,
                          const char *message)
{
    if (!generation_alive(generation)) return;
    pthread_mutex_lock(&g_lock);
    g_snapshot.state = state;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    pthread_mutex_unlock(&g_lock);
}

static int parse_devices(const char *body, bambu_cloud_device_t *devices)
{
    int count = 0;
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    cJSON *array = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    if (!cJSON_IsArray(array) && root) {
        cJSON *data = json_data(root);
        array = cJSON_GetObjectItemCaseSensitive(data, "devices");
    }
    cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, array) {
        if (count >= BAMBU_CLOUD_DEVICE_MAX) break;
        const char *serial = json_string(dev, "dev_id");
        if (!serial || !serial[0]) continue;
        const char *name = json_string(dev, "name");
        const char *model = json_string(dev, "dev_product_name");
        if (!model) model = json_string(dev, "dev_model_name");
        copy_text(devices[count].serial, sizeof(devices[count].serial), serial);
        copy_text(devices[count].name, sizeof(devices[count].name), name ? name : serial);
        copy_text(devices[count].model, sizeof(devices[count].model), model ? model : "");
        cJSON *online = cJSON_GetObjectItemCaseSensitive(dev, "online");
        devices[count].online = cJSON_IsTrue(online);
        count++;
    }
    cJSON_Delete(root);
    return count;
}

static bool fetch_devices(bambu_cloud_region_t region, const char *token,
                          bambu_cloud_device_t *devices, int *count,
                          char *message, size_t message_cap)
{
    http_result_t r;
    bool ok = http_request(site_host(region),
        "/api/v1/iot-service/api/user/bind", "GET", NULL,
        token, NULL, NULL, &r);
    if (!ok || r.status != 200) {
        response_message(&r, "已登录，但打印机列表获取失败", message, message_cap);
        http_free(&r);
        return false;
    }
    *count = parse_devices(r.body, devices);
    if (*count == 0)
        copy_text(message, message_cap, "登录成功，账号下没有找到打印机");
    else
        snprintf(message, message_cap, "登录成功，找到 %d 台打印机", *count);
    http_free(&r);
    return true;
}

static void publish_signed_in(worker_args_t *a, const char *token)
{
    if (!token || strlen(token) >= TOKEN_MAX) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "登录令牌过长，无法保存");
        return;
    }
    bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX] = {0};
    int count = 0;
    char message[192];
    fetch_devices(a->region, token, devices, &count, message, sizeof(message));
    char user_id[96] = {0};
    bool has_user_id = resolve_user_id(a->region, token, user_id, sizeof(user_id));
    if (!generation_alive(a->generation)) return;
    if (!secret_save(a->account, token, a->region)) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED,
                      "登录成功，但令牌保存失败");
        return;
    }
    pthread_mutex_lock(&g_lock);
    copy_text(g_token, sizeof(g_token), token);
    copy_text(g_user_id, sizeof(g_user_id), user_id);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), a->account);
    g_snapshot.region = a->region;
    g_snapshot.device_count = count;
    memcpy(g_snapshot.devices, devices, sizeof(devices));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    g_tfa_key[0] = 0;
    pthread_mutex_unlock(&g_lock);
    wipe(user_id, sizeof(user_id));
    if (!has_user_id)
        publish_state(a->generation, BAMBU_CLOUD_SIGNED_IN,
                      "已登录，但无法取得云端监视身份");
}

static void handle_login_reply(worker_args_t *a, http_result_t *r)
{
    char error[192];
    if (r->status != 200) {
        response_message(r, "登录被拓竹服务拒绝", error, sizeof(error));
        publish_state(a->generation, BAMBU_CLOUD_FAILED, error);
        return;
    }
    cJSON *root = r->body ? cJSON_Parse(r->body) : NULL;
    if (!root) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "无法读取登录响应");
        return;
    }
    const char *token = extract_token(root);
    if (token && token[0]) {
        char token_copy[TOKEN_MAX];
        copy_text(token_copy, sizeof(token_copy), token);
        cJSON_Delete(root);
        publish_signed_in(a, token_copy);
        wipe(token_copy, sizeof(token_copy));
        return;
    }
    cJSON *data = json_data(root);
    const char *login_type = json_string(data, "loginType");
    if (!login_type) login_type = json_string(root, "loginType");
    const char *tfa_key = json_string(data, "tfaKey");
    if (!tfa_key) tfa_key = json_string(root, "tfaKey");
    if ((login_type && strcmp(login_type, "tfa") == 0) || (tfa_key && tfa_key[0])) {
        if (!tfa_key || !tfa_key[0])
            publish_state(a->generation, BAMBU_CLOUD_FAILED, "拓竹要求双重验证，但没有返回验证会话");
        else {
            pthread_mutex_lock(&g_lock);
            copy_text(g_tfa_key, sizeof(g_tfa_key), tfa_key);
            pthread_mutex_unlock(&g_lock);
            publish_state(a->generation, BAMBU_CLOUD_NEED_TFA,
                          "请输入验证器应用中的 6 位验证码");
        }
    } else if (login_type && strcmp(login_type, "verifyCode") == 0) {
        publish_state(a->generation, BAMBU_CLOUD_NEED_CODE,
                      "验证码已发送，请输入验证码");
    } else {
        response_message(r, "登录响应中没有令牌", error, sizeof(error));
        publish_state(a->generation, BAMBU_CLOUD_FAILED, error);
    }
    cJSON_Delete(root);
}

static char *make_login_json(const char *account, const char *key,
                             const char *value)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "account", account);
    cJSON_AddStringToObject(root, key, value);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void *worker_main(void *context)
{
    worker_args_t *a = context;

    if (a->op == OP_REFRESH) {
        bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX] = {0};
        int count = 0;
        char message[192];
        fetch_devices(a->region, a->token, devices, &count,
                      message, sizeof(message));
        if (generation_alive(a->generation)) {
            pthread_mutex_lock(&g_lock);
            /* 设备列表刷新失败不会注销已经持有的云会话；保留登录态，
               让定时刷新能够恢复。message 仍会把本次失败展示给用户。 */
            g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
            g_snapshot.device_count = count;
            memcpy(g_snapshot.devices, devices, sizeof(devices));
            copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
            pthread_mutex_unlock(&g_lock);
        }
        goto done;
    }

    if (a->op == OP_PASSWORD || a->op == OP_REQUEST_EMAIL_CODE ||
        a->op == OP_REQUEST_SMS_CODE) {
        cJSON *root = cJSON_CreateObject();
        const bool password_login = a->op == OP_PASSWORD;
        const bool sms_code = a->op == OP_REQUEST_SMS_CODE;
        cJSON_AddStringToObject(root, password_login ? "account" :
                                     (sms_code ? "phone" : "email"), a->account);
        cJSON_AddStringToObject(root, password_login ? "password" : "type",
                               password_login ? a->password : "codeLogin");
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        http_result_t r = {0};
        bool ok = json && http_request(api_host(a->region),
            password_login ? "/v1/user-service/user/login" :
            (sms_code ? "/v1/user-service/user/sendsmscode"
                      : "/v1/user-service/user/sendemail/code"),
            "POST", json, NULL, NULL, NULL, &r);
        cJSON_free(json);
        if (!ok) {
            char msg[192]; response_message(&r, "无法连接拓竹登录服务", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        } else if (password_login) {
            handle_login_reply(a, &r);
        } else if (r.status == 200) {
            publish_state(a->generation, BAMBU_CLOUD_NEED_CODE,
                          sms_code ? "验证码已发送到手机，请输入验证码"
                                   : "验证码已发送到邮箱，请输入验证码");
        } else {
            char msg[192]; response_message(&r, "验证码发送失败", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        }
        http_free(&r);
        goto done;
    }

    if (a->op == OP_SUBMIT_CODE) {
        http_result_t r = {0};
        if (a->challenge == BAMBU_CLOUD_NEED_CODE) {
            char *json = make_login_json(a->account, "code", a->code);
            bool ok = json && http_request(api_host(a->region),
                "/v1/user-service/user/login", "POST", json,
                NULL, NULL, NULL, &r);
            cJSON_free(json);
            if (!ok) {
                char msg[192]; response_message(&r, "验证码提交失败", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            } else if (r.status == 200) {
                handle_login_reply(a, &r);
            } else {
                char msg[192]; response_message(&r, "验证码不正确", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            }
        } else {
            http_result_t csrf_r = {0};
            bool csrf_ok = http_request(site_host(a->region), "/api/csrf",
                                        "GET", NULL, NULL, "", NULL, &csrf_r);
            char csrf[1024] = {0};
            cookie_value(csrf_r.cookies, "bbl_csrf_token", csrf, sizeof(csrf));
            if (!csrf_ok || csrf_r.status != 200 || !csrf[0]) {
                char msg[192]; response_message(&csrf_r, "无法建立双重验证会话", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_TFA, msg);
                http_free(&csrf_r);
                goto code_done;
            }
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "tfaKey", a->tfa_key);
            cJSON_AddStringToObject(root, "tfaCode", a->code);
            char *json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            bool ok = json && http_request(site_host(a->region),
                "/api/sign-in/tfa", "POST", json, NULL,
                csrf_r.cookies, csrf, &r);
            cJSON_free(json);
            http_free(&csrf_r);
            if (!ok || r.status != 200) {
                char msg[192]; response_message(&r, "验证器验证码不正确", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_TFA, msg);
            } else {
                cJSON *reply = r.body ? cJSON_Parse(r.body) : NULL;
                const char *token = reply ? extract_token(reply) : NULL;
                char token_cookie[TOKEN_MAX] = {0};
                if (!token) {
                    cookie_value(r.cookies, "token", token_cookie, sizeof(token_cookie));
                    token = token_cookie;
                }
                if (token && token[0]) publish_signed_in(a, token);
                else publish_state(a->generation, BAMBU_CLOUD_NEED_TFA,
                                   "验证码已通过，但登录服务没有返回令牌");
                cJSON_Delete(reply);
                wipe(token_cookie, sizeof(token_cookie));
            }
        }
code_done:
        http_free(&r);
    }

done:
    wipe(a->password, sizeof(a->password));
    wipe(a->token, sizeof(a->token));
    wipe(a->tfa_key, sizeof(a->tfa_key));
    free(a);
    atomic_store(&g_busy, 0);
    return NULL;
}

static bool start_worker(worker_args_t *a)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_busy, &expected, 1)) {
        free(a);
        return false;
    }
    a->generation = atomic_load(&g_generation);
    if (a->op == OP_REFRESH) {
        pthread_mutex_lock(&g_lock);
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), "正在刷新打印机列表…");
        pthread_mutex_unlock(&g_lock);
    } else {
        publish_state(a->generation, BAMBU_CLOUD_BUSY, "正在连接拓竹云服务…");
    }
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, a) != 0) {
        atomic_store(&g_busy, 0);
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "无法启动登录任务");
        free(a);
        return false;
    }
    pthread_detach(th);
    return true;
}

void bambu_cloud_snapshot(bambu_cloud_snapshot_t *out)
{
    if (!out) return;
    bambu_cloud_init();
    pthread_mutex_lock(&g_lock);
    *out = g_snapshot;
    pthread_mutex_unlock(&g_lock);
}

bool bambu_cloud_copy_mqtt_credentials(char *user_id, size_t user_id_cap,
                                       char *token, size_t token_cap,
                                       bambu_cloud_region_t *region)
{
    if (!user_id || !user_id_cap || !token || !token_cap || !region) return false;
    bambu_cloud_init();
    pthread_mutex_lock(&g_lock);
    bool ok = g_snapshot.state == BAMBU_CLOUD_SIGNED_IN &&
              g_token[0] && strlen(g_token) < token_cap;
    if (ok) {
        copy_text(user_id, user_id_cap, g_user_id);
        copy_text(token, token_cap, g_token);
        *region = g_snapshot.region;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool bambu_cloud_resolve_mqtt_user_id(const char *token,
                                      bambu_cloud_region_t region,
                                      char *user_id, size_t user_id_cap)
{
    if (!token || !token[0] || !user_id || !user_id_cap) return false;
    bool ok = resolve_user_id(region, token, user_id, user_id_cap);
    if (ok) {
        bambu_cloud_init();
        pthread_mutex_lock(&g_lock);
        if (strcmp(g_token, token) == 0)
            copy_text(g_user_id, sizeof(g_user_id), user_id);
        pthread_mutex_unlock(&g_lock);
    }
    return ok;
}

bool bambu_cloud_set_region(bambu_cloud_region_t region)
{
    bambu_cloud_init();
    if (region != BAMBU_CLOUD_REGION_GLOBAL && region != BAMBU_CLOUD_REGION_CHINA)
        return false;
    pthread_mutex_lock(&g_lock);
    bool ok = g_snapshot.state != BAMBU_CLOUD_BUSY &&
              g_snapshot.state != BAMBU_CLOUD_SIGNED_IN;
    if (ok) g_snapshot.region = region;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static worker_args_t *new_worker(worker_op_t op)
{
    bambu_cloud_init();
    worker_args_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->op = op;
    pthread_mutex_lock(&g_lock);
    a->region = g_snapshot.region;
    copy_text(a->account, sizeof(a->account), g_snapshot.account);
    pthread_mutex_unlock(&g_lock);
    return a;
}

bool bambu_cloud_login_password(const char *account, const char *password)
{
    if (!account || !account[0] || !password || !password[0]) return false;
    worker_args_t *a = new_worker(OP_PASSWORD);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), account);
    copy_text(a->password, sizeof(a->password), password);
    pthread_mutex_lock(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), account);
    pthread_mutex_unlock(&g_lock);
    return start_worker(a);
}

bool bambu_cloud_request_email_code(const char *email)
{
    if (!email || !email[0]) return false;
    worker_args_t *a = new_worker(OP_REQUEST_EMAIL_CODE);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), email);
    pthread_mutex_lock(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), email);
    pthread_mutex_unlock(&g_lock);
    return start_worker(a);
}

bool bambu_cloud_request_sms_code(const char *phone)
{
    if (!phone || !phone[0]) return false;
    worker_args_t *a = new_worker(OP_REQUEST_SMS_CODE);
    if (!a) return false;
    if (a->region != BAMBU_CLOUD_REGION_CHINA) {
        free(a);
        return false;
    }
    copy_text(a->account, sizeof(a->account), phone);
    pthread_mutex_lock(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), phone);
    pthread_mutex_unlock(&g_lock);
    return start_worker(a);
}

bool bambu_cloud_submit_code(const char *code)
{
    if (!code || !code[0]) return false;
    worker_args_t *a = new_worker(OP_SUBMIT_CODE);
    if (!a) return false;
    copy_text(a->code, sizeof(a->code), code);
    pthread_mutex_lock(&g_lock);
    a->challenge = g_snapshot.state;
    copy_text(a->tfa_key, sizeof(a->tfa_key), g_tfa_key);
    pthread_mutex_unlock(&g_lock);
    if (a->challenge != BAMBU_CLOUD_NEED_CODE &&
        a->challenge != BAMBU_CLOUD_NEED_TFA) {
        free(a); return false;
    }
    return start_worker(a);
}

bool bambu_cloud_refresh_devices(void)
{
    worker_args_t *a = new_worker(OP_REFRESH);
    if (!a) return false;
    pthread_mutex_lock(&g_lock);
    bool signed_in = g_snapshot.state == BAMBU_CLOUD_SIGNED_IN;
    copy_text(a->token, sizeof(a->token), g_token);
    pthread_mutex_unlock(&g_lock);
    if (!signed_in || !a->token[0]) { free(a); return false; }
    return start_worker(a);
}

void bambu_cloud_logout(void)
{
    bambu_cloud_init();
    atomic_fetch_add(&g_generation, 1);
    bsp_conf_write("bambu_cloud.secret", "");
    pthread_mutex_lock(&g_lock);
    bambu_cloud_region_t region = g_snapshot.region;
    wipe(g_token, sizeof(g_token));
    wipe(g_user_id, sizeof(g_user_id));
    wipe(g_tfa_key, sizeof(g_tfa_key));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_OUT;
    g_snapshot.region = region;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "请输入拓竹账号");
    pthread_mutex_unlock(&g_lock);
}
