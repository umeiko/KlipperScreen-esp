#define WIN32_LEAN_AND_MEAN
#include "bambu_cloud.h"
#include "bambu_cloud_internal.h"
#include "bsp_conf.h"
#include "cJSON.h"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_MAX       2048
#define TFA_KEY_MAX      512
#define HTTP_BODY_MAX (256 * 1024)

typedef struct {
    DWORD status;
    char *body;
    char cookies[4096];
    DWORD win_error;
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
    LONG generation;
    bambu_cloud_region_t region;
    bambu_cloud_state_t challenge;
    char account[BAMBU_CLOUD_ACCOUNT_MAX];
    char password[192];
    char code[32];
    char token[TOKEN_MAX];
    char tfa_key[TFA_KEY_MAX];
} worker_args_t;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
static bambu_cloud_snapshot_t g_snapshot;
static char g_token[TOKEN_MAX];
static char g_user_id[96];
static char g_tfa_key[TFA_KEY_MAX];
static volatile LONG g_busy;
static volatile LONG g_generation = 1;

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static const wchar_t *api_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? L"api.bambulab.cn" : L"api.bambulab.com";
}

static const wchar_t *site_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? L"bambulab.cn" : L"bambulab.com";
}

static wchar_t *wide_from_utf8(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *out = calloc((size_t)n, sizeof(wchar_t));
    if (out) MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

static char *utf8_from_wide(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = malloc((size_t)n);
    if (out) WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}

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

static void collect_cookies(HINTERNET request, char *jar, size_t cap)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, NULL, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes) return;
    wchar_t *raw = malloc(bytes);
    if (!raw) return;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, raw, &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        free(raw);
        return;
    }
    char *headers = utf8_from_wide(raw);
    free(raw);
    if (!headers) return;
    for (char *line = headers; line && *line;) {
        char *end = strstr(line, "\r\n");
        if (end) *end = 0;
        if (_strnicmp(line, "Set-Cookie:", 11) == 0) {
            const char *value = line + 11;
            while (*value == ' ' || *value == '\t') value++;
            cookie_put(jar, cap, value);
        }
        if (!end) break;
        line = end + 2;
    }
    free(headers);
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

static bool http_request(HINTERNET session, const wchar_t *host,
                         const wchar_t *path, const wchar_t *method,
                         const char *json, const char *bearer,
                         const char *cookie_jar, const char *csrf,
                         http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) { out->win_error = GetLastError(); return false; }
    HINTERNET request = WinHttpOpenRequest(connection, method, path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        out->win_error = GetLastError();
        WinHttpCloseHandle(connection);
        return false;
    }
    DWORD disable_cookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                     &disable_cookies, sizeof(disable_cookies));

    WinHttpAddRequestHeaders(request,
        L"Accept: application/json\r\nContent-Type: application/json\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    char extra[8192] = {0};
    if (bearer && bearer[0])
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 "Authorization: Bearer %s\r\n", bearer);
    if (cookie_jar && cookie_jar[0])
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 "Cookie: %s\r\n", cookie_jar);
    if (csrf && csrf[0])
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 "x-bbl-csrf-token: %s\r\n", csrf);
    if (extra[0]) {
        wchar_t *wextra = wide_from_utf8(extra);
        if (wextra) {
            WinHttpAddRequestHeaders(request, wextra, (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            free(wextra);
        }
    }

    DWORD body_len = json ? (DWORD)strlen(json) : 0;
    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)json, body_len, body_len, 0) &&
              WinHttpReceiveResponse(request, NULL);
    if (!ok) {
        out->win_error = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        return false;
    }
    DWORD status_size = sizeof(out->status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &out->status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (cookie_jar) copy_text(out->cookies, sizeof(out->cookies), cookie_jar);
    collect_cookies(request, out->cookies, sizeof(out->cookies));

    size_t used = 0, cap = 4096;
    out->body = malloc(cap);
    if (!out->body) ok = FALSE;
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            out->win_error = GetLastError(); ok = FALSE; break;
        }
        if (!available) break;
        if (used + available + 1 > HTTP_BODY_MAX) {
            out->win_error = ERROR_INSUFFICIENT_BUFFER; ok = FALSE; break;
        }
        if (used + available + 1 > cap) {
            size_t next = cap;
            while (next < used + available + 1) next *= 2;
            char *grown = realloc(out->body, next);
            if (!grown) { ok = FALSE; break; }
            out->body = grown; cap = next;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, out->body + used, available, &got)) {
            out->win_error = GetLastError(); ok = FALSE; break;
        }
        used += got;
    }
    if (out->body) out->body[used] = 0;
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    return ok != FALSE;
}

static void http_free(http_result_t *r)
{
    free(r->body);
    r->body = NULL;
}

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
    SecureZeroMemory(raw, sizeof(raw));
    return out[0] != 0;
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

    DWORD decoded_len = 0;
    BOOL ok = CryptStringToBinaryA(encoded, (DWORD)padded_len,
                                    CRYPT_STRING_BASE64, NULL, &decoded_len,
                                    NULL, NULL);
    BYTE *decoded = ok ? malloc((size_t)decoded_len + 1) : NULL;
    if (decoded)
        ok = CryptStringToBinaryA(encoded, (DWORD)padded_len,
                                  CRYPT_STRING_BASE64, decoded, &decoded_len,
                                  NULL, NULL);
    SecureZeroMemory(encoded, padded_len);
    free(encoded);
    if (!ok || !decoded) { free(decoded); return false; }
    decoded[decoded_len] = 0;
    cJSON *payload = cJSON_ParseWithLength((const char *)decoded, decoded_len);
    SecureZeroMemory(decoded, (size_t)decoded_len + 1);
    free(decoded);
    bool found = copy_uid_value(payload, out, cap);
    cJSON_Delete(payload);
    return found;
}

static bool resolve_user_id(HINTERNET session, bambu_cloud_region_t region,
                            const char *token, char *out, size_t cap)
{
    if (jwt_user_id(token, out, cap)) return true;
    http_result_t r = {0};
    bool ok = http_request(session, api_host(region),
                           L"/v1/user-service/my/profile", L"GET", NULL,
                           token, NULL, NULL, &r);
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
    else if (r->status) snprintf(out, cap, "%s（HTTP %lu）", fallback, (unsigned long)r->status);
    else if (r->win_error) snprintf(out, cap, "%s（Windows %lu）", fallback, (unsigned long)r->win_error);
    else copy_text(out, cap, fallback);
    cJSON_Delete(root);
}

static bool secret_save(const char *account, const char *token,
                        bambu_cloud_region_t region)
{
    size_t plain_len = strlen(account) + strlen(token) + 8;
    char *plain = malloc(plain_len);
    if (!plain) return false;
    snprintf(plain, plain_len, "%d\n%s\n%s", (int)region, account, token);
    DATA_BLOB in = {(DWORD)strlen(plain), (BYTE *)plain}, encrypted = {0};
    BOOL ok = CryptProtectData(&in, L"Klipper Remote Bambu Cloud", NULL, NULL, NULL,
                               CRYPTPROTECT_UI_FORBIDDEN, &encrypted);
    SecureZeroMemory(plain, plain_len);
    free(plain);
    if (!ok) return false;

    char *hex = malloc((size_t)encrypted.cbData * 2 + 2);
    if (!hex) { LocalFree(encrypted.pbData); return false; }
    for (DWORD i = 0; i < encrypted.cbData; i++)
        sprintf(hex + i * 2, "%02x", encrypted.pbData[i]);
    hex[encrypted.cbData * 2] = '\n';
    hex[encrypted.cbData * 2 + 1] = 0;
    int rc = bsp_conf_write("bambu_cloud.secret", hex);
    SecureZeroMemory(hex, (size_t)encrypted.cbData * 2);
    free(hex);
    LocalFree(encrypted.pbData);
    return rc == 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool secret_load(char *account, size_t account_cap, char *token,
                        size_t token_cap, bambu_cloud_region_t *region)
{
    char *hex = malloc(8192);
    if (!hex) return false;
    int n = bsp_conf_read("bambu_cloud.secret", hex, 8192);
    if (n <= 1) { free(hex); return false; }
    while (n > 0 && (hex[n - 1] == '\r' || hex[n - 1] == '\n')) n--;
    if ((n & 1) != 0) { free(hex); return false; }
    BYTE *bytes = malloc((size_t)n / 2);
    if (!bytes) { free(hex); return false; }
    for (int i = 0; i < n; i += 2) {
        int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) { free(bytes); free(hex); return false; }
        bytes[i / 2] = (BYTE)((hi << 4) | lo);
    }
    DATA_BLOB in = {(DWORD)n / 2, bytes}, plain = {0};
    BOOL ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                                 CRYPTPROTECT_UI_FORBIDDEN, &plain);
    SecureZeroMemory(bytes, (size_t)n / 2);
    free(bytes); free(hex);
    if (!ok || !plain.pbData) return false;

    char *text = malloc((size_t)plain.cbData + 1);
    if (!text) { LocalFree(plain.pbData); return false; }
    memcpy(text, plain.pbData, plain.cbData); text[plain.cbData] = 0;
    LocalFree(plain.pbData);
    char *line1 = text, *line2 = strchr(line1, '\n');
    char *line3 = line2 ? strchr(line2 + 1, '\n') : NULL;
    if (!line2 || !line3) ok = FALSE;
    else {
        *line2 = 0; *line3 = 0;
        *region = atoi(line1) == BAMBU_CLOUD_REGION_CHINA
                ? BAMBU_CLOUD_REGION_CHINA : BAMBU_CLOUD_REGION_GLOBAL;
        copy_text(account, account_cap, line2 + 1);
        copy_text(token, token_cap, line3 + 1);
        ok = account[0] && token[0];
    }
    SecureZeroMemory(text, (size_t)plain.cbData + 1);
    free(text);
    return ok != FALSE;
}

static BOOL CALLBACK initialize_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
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
    return TRUE;
}

void bambu_cloud_init(void)
{
    InitOnceExecuteOnce(&g_once, initialize_once, NULL, NULL);
}

static bool generation_alive(LONG generation)
{
    return InterlockedCompareExchange(&g_generation, 0, 0) == generation;
}

static void publish_state(LONG generation, bambu_cloud_state_t state,
                          const char *message)
{
    if (!generation_alive(generation)) return;
    EnterCriticalSection(&g_lock);
    g_snapshot.state = state;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    LeaveCriticalSection(&g_lock);
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

static bool fetch_devices(HINTERNET session, bambu_cloud_region_t region,
                          const char *token, bambu_cloud_device_t *devices,
                          int *count, char *message, size_t message_cap)
{
    http_result_t r;
    bool ok = http_request(session, site_host(region),
        L"/api/v1/iot-service/api/user/bind", L"GET", NULL,
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

static void publish_signed_in(worker_args_t *a, const char *token, HINTERNET session)
{
    if (!token || strlen(token) >= TOKEN_MAX) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "登录令牌过长，无法保存");
        return;
    }
    bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX] = {0};
    int count = 0;
    char message[192];
    fetch_devices(session, a->region, token, devices, &count,
                   message, sizeof(message));
    char user_id[96] = {0};
    bool has_user_id = resolve_user_id(session, a->region, token,
                                       user_id, sizeof(user_id));
    if (!generation_alive(a->generation)) return;
    if (!secret_save(a->account, token, a->region)) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED,
                      "登录成功，但加密令牌保存失败");
        return;
    }
    EnterCriticalSection(&g_lock);
    copy_text(g_token, sizeof(g_token), token);
    copy_text(g_user_id, sizeof(g_user_id), user_id);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), a->account);
    g_snapshot.region = a->region;
    g_snapshot.device_count = count;
    memcpy(g_snapshot.devices, devices, sizeof(devices));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    g_tfa_key[0] = 0;
    LeaveCriticalSection(&g_lock);
    SecureZeroMemory(user_id, sizeof(user_id));
    if (!has_user_id)
        publish_state(a->generation, BAMBU_CLOUD_SIGNED_IN,
                      "已登录，但无法取得云端监视身份");
}

static void handle_login_reply(worker_args_t *a, HINTERNET session,
                               http_result_t *r)
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
        publish_signed_in(a, token_copy, session);
        SecureZeroMemory(token_copy, sizeof(token_copy));
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
            EnterCriticalSection(&g_lock);
            copy_text(g_tfa_key, sizeof(g_tfa_key), tfa_key);
            LeaveCriticalSection(&g_lock);
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

static DWORD WINAPI worker_main(LPVOID context)
{
    worker_args_t *a = context;
    HINTERNET session = WinHttpOpen(L"bambu_network_agent/01.09.05.01",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "无法初始化 Windows 网络服务");
        goto done;
    }
    WinHttpSetTimeouts(session, 6000, 6000, 10000, 10000);

    if (a->op == OP_REFRESH) {
        bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX] = {0};
        int count = 0;
        char message[192];
        fetch_devices(session, a->region, a->token, devices, &count,
                      message, sizeof(message));
        if (generation_alive(a->generation)) {
            EnterCriticalSection(&g_lock);
            /* 设备列表刷新失败不会注销已经持有的云会话；保留登录态，
               让定时刷新能够恢复。message 仍会把本次失败展示给用户。 */
            g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
            g_snapshot.device_count = count;
            memcpy(g_snapshot.devices, devices, sizeof(devices));
            copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
            LeaveCriticalSection(&g_lock);
        }
        goto done_session;
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
        bool ok = json && http_request(session, api_host(a->region),
            password_login ? L"/v1/user-service/user/login" :
            (sms_code ? L"/v1/user-service/user/sendsmscode"
                      : L"/v1/user-service/user/sendemail/code"),
            L"POST", json, NULL, NULL, NULL, &r);
        cJSON_free(json);
        if (!ok) {
            char msg[192]; response_message(&r, "无法连接拓竹登录服务", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        } else if (password_login) {
            handle_login_reply(a, session, &r);
        } else if (r.status == 200) {
            publish_state(a->generation, BAMBU_CLOUD_NEED_CODE,
                          sms_code ? "验证码已发送到手机，请输入验证码"
                                   : "验证码已发送到邮箱，请输入验证码");
        } else {
            char msg[192]; response_message(&r, "验证码发送失败", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        }
        http_free(&r);
        goto done_session;
    }

    if (a->op == OP_SUBMIT_CODE) {
        http_result_t r = {0};
        if (a->challenge == BAMBU_CLOUD_NEED_CODE) {
            char *json = make_login_json(a->account, "code", a->code);
            bool ok = json && http_request(session, api_host(a->region),
                L"/v1/user-service/user/login", L"POST", json,
                NULL, NULL, NULL, &r);
            cJSON_free(json);
            if (!ok) {
                char msg[192]; response_message(&r, "验证码提交失败", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            } else if (r.status == 200) {
                handle_login_reply(a, session, &r);
            } else {
                char msg[192]; response_message(&r, "验证码不正确", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            }
        } else {
            http_result_t csrf_r = {0};
            bool csrf_ok = http_request(session, site_host(a->region), L"/api/csrf",
                                        L"GET", NULL, NULL, "", NULL, &csrf_r);
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
            bool ok = json && http_request(session, site_host(a->region),
                L"/api/sign-in/tfa", L"POST", json, NULL,
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
                if (token && token[0]) publish_signed_in(a, token, session);
                else publish_state(a->generation, BAMBU_CLOUD_NEED_TFA,
                                   "验证码已通过，但登录服务没有返回令牌");
                cJSON_Delete(reply);
                SecureZeroMemory(token_cookie, sizeof(token_cookie));
            }
        }
code_done:
        http_free(&r);
    }

done_session:
    WinHttpCloseHandle(session);
done:
    SecureZeroMemory(a->password, sizeof(a->password));
    SecureZeroMemory(a->token, sizeof(a->token));
    SecureZeroMemory(a->tfa_key, sizeof(a->tfa_key));
    free(a);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static bool start_worker(worker_args_t *a)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        free(a);
        return false;
    }
    a->generation = InterlockedCompareExchange(&g_generation, 0, 0);
    if (a->op == OP_REFRESH) {
        EnterCriticalSection(&g_lock);
        copy_text(g_snapshot.message, sizeof(g_snapshot.message), "正在刷新打印机列表…");
        LeaveCriticalSection(&g_lock);
    } else {
        publish_state(a->generation, BAMBU_CLOUD_BUSY, "正在连接拓竹云服务…");
    }
    HANDLE thread = CreateThread(NULL, 0, worker_main, a, 0, NULL);
    if (!thread) {
        InterlockedExchange(&g_busy, 0);
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "无法启动登录任务");
        free(a);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void bambu_cloud_snapshot(bambu_cloud_snapshot_t *out)
{
    if (!out) return;
    bambu_cloud_init();
    EnterCriticalSection(&g_lock);
    *out = g_snapshot;
    LeaveCriticalSection(&g_lock);
}

bool bambu_cloud_copy_mqtt_credentials(char *user_id, size_t user_id_cap,
                                       char *token, size_t token_cap,
                                       bambu_cloud_region_t *region)
{
    if (!user_id || !user_id_cap || !token || !token_cap || !region) return false;
    bambu_cloud_init();
    EnterCriticalSection(&g_lock);
    bool ok = g_snapshot.state == BAMBU_CLOUD_SIGNED_IN &&
              g_token[0] && strlen(g_token) < token_cap;
    if (ok) {
        copy_text(user_id, user_id_cap, g_user_id);
        copy_text(token, token_cap, g_token);
        *region = g_snapshot.region;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

bool bambu_cloud_resolve_mqtt_user_id(const char *token,
                                      bambu_cloud_region_t region,
                                      char *user_id, size_t user_id_cap)
{
    if (!token || !token[0] || !user_id || !user_id_cap) return false;
    HINTERNET session = WinHttpOpen(L"bambu_network_agent/01.09.05.01",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 6000, 6000, 10000, 10000);
    bool ok = resolve_user_id(session, region, token, user_id, user_id_cap);
    WinHttpCloseHandle(session);
    if (ok) {
        bambu_cloud_init();
        EnterCriticalSection(&g_lock);
        if (strcmp(g_token, token) == 0)
            copy_text(g_user_id, sizeof(g_user_id), user_id);
        LeaveCriticalSection(&g_lock);
    }
    return ok;
}

bool bambu_cloud_set_region(bambu_cloud_region_t region)
{
    bambu_cloud_init();
    if (region != BAMBU_CLOUD_REGION_GLOBAL && region != BAMBU_CLOUD_REGION_CHINA)
        return false;
    EnterCriticalSection(&g_lock);
    bool ok = g_snapshot.state != BAMBU_CLOUD_BUSY &&
              g_snapshot.state != BAMBU_CLOUD_SIGNED_IN;
    if (ok) g_snapshot.region = region;
    LeaveCriticalSection(&g_lock);
    return ok;
}

static worker_args_t *new_worker(worker_op_t op)
{
    bambu_cloud_init();
    worker_args_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->op = op;
    EnterCriticalSection(&g_lock);
    a->region = g_snapshot.region;
    copy_text(a->account, sizeof(a->account), g_snapshot.account);
    LeaveCriticalSection(&g_lock);
    return a;
}

bool bambu_cloud_login_password(const char *account, const char *password)
{
    if (!account || !account[0] || !password || !password[0]) return false;
    worker_args_t *a = new_worker(OP_PASSWORD);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), account);
    copy_text(a->password, sizeof(a->password), password);
    EnterCriticalSection(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), account);
    LeaveCriticalSection(&g_lock);
    return start_worker(a);
}

bool bambu_cloud_request_email_code(const char *email)
{
    if (!email || !email[0]) return false;
    worker_args_t *a = new_worker(OP_REQUEST_EMAIL_CODE);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), email);
    EnterCriticalSection(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), email);
    LeaveCriticalSection(&g_lock);
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
    EnterCriticalSection(&g_lock);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), phone);
    LeaveCriticalSection(&g_lock);
    return start_worker(a);
}

bool bambu_cloud_submit_code(const char *code)
{
    if (!code || !code[0]) return false;
    worker_args_t *a = new_worker(OP_SUBMIT_CODE);
    if (!a) return false;
    copy_text(a->code, sizeof(a->code), code);
    EnterCriticalSection(&g_lock);
    a->challenge = g_snapshot.state;
    copy_text(a->tfa_key, sizeof(a->tfa_key), g_tfa_key);
    LeaveCriticalSection(&g_lock);
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
    EnterCriticalSection(&g_lock);
    bool signed_in = g_snapshot.state == BAMBU_CLOUD_SIGNED_IN;
    copy_text(a->token, sizeof(a->token), g_token);
    LeaveCriticalSection(&g_lock);
    if (!signed_in || !a->token[0]) { free(a); return false; }
    return start_worker(a);
}

void bambu_cloud_logout(void)
{
    bambu_cloud_init();
    InterlockedIncrement(&g_generation);
    bsp_conf_write("bambu_cloud.secret", "");
    EnterCriticalSection(&g_lock);
    bambu_cloud_region_t region = g_snapshot.region;
    SecureZeroMemory(g_token, sizeof(g_token));
    SecureZeroMemory(g_user_id, sizeof(g_user_id));
    SecureZeroMemory(g_tfa_key, sizeof(g_tfa_key));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_OUT;
    g_snapshot.region = region;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "请输入拓竹账号");
    LeaveCriticalSection(&g_lock);
}
