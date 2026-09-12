/*
 * bambu_net actor + cloud snapshot for ESP32.
 *
 * Protocol (hosts, paths, JSON fields, TFA Cookie/CSRF dance, token/UID
 * extraction, message keys) mirrors src/ports/desktop/bambu_cloud_winhttp.c
 * exactly; only the transport and credential store differ.
 *
 * Logs never contain request/response bodies, Authorization, Cookie, CSRF,
 * tokens, passwords, codes or full user ids.
 */
#include "bambu_runtime_esp32.h"
#include "bambu_http_esp32.h"
#include "bambu_secret_esp32.h"

#include "bsp_wifi.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bambu_cloud";

#define TOKEN_MAX    BAMBU_SECRET_TOKEN_MAX   /* 2048 */
#define TFA_KEY_MAX  512
#define QUEUE_DEPTH  4
#define ACTOR_STACK  10240

typedef enum {
    OP_PASSWORD,
    OP_REQUEST_EMAIL_CODE,
    OP_REQUEST_SMS_CODE,
    OP_SUBMIT_CODE,
    OP_REFRESH,
    OP_ERASE_SECRET,             /* serialized NVS wipe after logout */
} op_t;

typedef struct {
    op_t op;
    uint32_t generation;
    bambu_cloud_region_t region;
    bambu_cloud_state_t challenge;
    char account[BAMBU_CLOUD_ACCOUNT_MAX];
    char password[192];
    char code[32];
    char token[TOKEN_MAX];
    char tfa_key[TFA_KEY_MAX];
} cmd_t;

static StaticSemaphore_t g_mutex_buf;
static SemaphoreHandle_t g_mutex;
static QueueHandle_t g_queue;
static bambu_cloud_snapshot_t g_snapshot;
static char g_token[TOKEN_MAX];
static char g_user_id[96];
static char g_tfa_key[TFA_KEY_MAX];
static volatile uint32_t g_generation = 1;
static int g_outstanding;               /* guarded by g_mutex */
static bool g_ready;

/* ------------------------------------------------------------------ */

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *api_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? "api.bambulab.cn"
                                              : "api.bambulab.com";
}

static const char *site_host(bambu_cloud_region_t region)
{
    return region == BAMBU_CLOUD_REGION_CHINA ? "bambulab.cn"
                                              : "bambulab.com";
}

static bool generation_alive(uint32_t generation)
{
    return g_generation == generation;
}

/* Snapshot mutex is held only for plain memory copies. */
static void publish_state(uint32_t generation, bambu_cloud_state_t state,
                          const char *message)
{
    if (!generation_alive(generation)) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot.state = state;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    xSemaphoreGive(g_mutex);
}

/* ------------------------- JSON helpers --------------------------- */

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
    bambu_http_wipe(raw, sizeof(raw));
    return out[0] != 0;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

static bool jwt_user_id(const char *token, char *out, size_t cap)
{
    const char *dot1 = token ? strchr(token, '.') : NULL;
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2 || dot2 <= dot1 + 1) return false;
    const char *src = dot1 + 1;
    size_t src_len = (size_t)(dot2 - src);
    size_t dec_cap = src_len / 4 * 3 + 4;
    unsigned char *dec = malloc(dec_cap);
    if (!dec) return false;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        int v = b64_val(src[i]);
        if (v < 0) break;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dec[n++] = (unsigned char)(acc >> bits);
        }
    }
    bool found = false;
    if (n) {
        cJSON *payload = cJSON_ParseWithLength((const char *)dec, n);
        found = copy_uid_value(payload, out, cap);
        cJSON_Delete(payload);
    }
    bambu_http_wipe(dec, dec_cap);
    free(dec);
    return found;
}

static bool resolve_user_id(bambu_cloud_region_t region, const char *token,
                            char *out, size_t cap)
{
    if (jwt_user_id(token, out, cap)) return true;
    bambu_http_result_t r;
    bool ok = bambu_http_request(api_host(region), "/v1/user-service/my/profile",
                                 HTTP_METHOD_GET, NULL, token, NULL, NULL, &r);
    if (!ok || r.status != 200) { bambu_http_free(&r); return false; }
    cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
    cJSON *data = root ? json_data(root) : NULL;
    bool found = copy_uid_value(root, out, cap) ||
                 (data != root && copy_uid_value(data, out, cap));
    cJSON_Delete(root);
    bambu_http_free(&r);
    return found;
}

static void response_message(bambu_http_result_t *r, const char *fallback,
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
    else if (r->esp_err != ESP_OK && r->esp_err != ESP_FAIL)
        snprintf(out, cap, "%s（%s）", fallback, esp_err_to_name(r->esp_err));
    else copy_text(out, cap, fallback);
    cJSON_Delete(root);
}

/* ------------------------- cloud operations ------------------------ */

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
        copy_text(devices[count].name, sizeof(devices[count].name),
                  name ? name : serial);
        copy_text(devices[count].model, sizeof(devices[count].model),
                  model ? model : "");
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
    bambu_http_result_t r;
    bool ok = bambu_http_request(site_host(region),
        "/api/v1/iot-service/api/user/bind", HTTP_METHOD_GET, NULL,
        token, NULL, NULL, &r);
    if (!ok || r.status != 200) {
        response_message(&r, "已登录，但打印机列表获取失败", message, message_cap);
        bambu_http_free(&r);
        return false;
    }
    *count = parse_devices(r.body, devices);
    if (*count == 0)
        copy_text(message, message_cap, "登录成功，账号下没有找到打印机");
    else
        snprintf(message, message_cap, "登录成功，找到 %d 台打印机", *count);
    bambu_http_free(&r);
    return true;
}

static void publish_signed_in(cmd_t *a, const char *token)
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
    if (!generation_alive(a->generation)) {
        bambu_http_wipe(user_id, sizeof(user_id));
        return;
    }
    if (!bambu_secret_save((uint32_t)a->region, a->account, user_id, token)) {
        bambu_http_wipe(user_id, sizeof(user_id));
        publish_state(a->generation, BAMBU_CLOUD_FAILED,
                      "登录成功，但加密令牌保存失败");
        return;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    copy_text(g_token, sizeof(g_token), token);
    copy_text(g_user_id, sizeof(g_user_id), user_id);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), a->account);
    g_snapshot.region = a->region;
    g_snapshot.device_count = count;
    memcpy(g_snapshot.devices, devices, sizeof(devices));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
    g_tfa_key[0] = 0;
    xSemaphoreGive(g_mutex);
    bambu_http_wipe(user_id, sizeof(user_id));
    if (!has_user_id)
        publish_state(a->generation, BAMBU_CLOUD_SIGNED_IN,
                      "已登录，但无法取得云端监视身份");
}

static void handle_login_reply(cmd_t *a, bambu_http_result_t *r)
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
        bambu_http_wipe(token_copy, sizeof(token_copy));
        return;
    }
    cJSON *data = json_data(root);
    const char *login_type = json_string(data, "loginType");
    if (!login_type) login_type = json_string(root, "loginType");
    const char *tfa_key = json_string(data, "tfaKey");
    if (!tfa_key) tfa_key = json_string(root, "tfaKey");
    if ((login_type && strcmp(login_type, "tfa") == 0) || (tfa_key && tfa_key[0])) {
        if (!tfa_key || !tfa_key[0])
            publish_state(a->generation, BAMBU_CLOUD_FAILED,
                          "拓竹要求双重验证，但没有返回验证会话");
        else {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            copy_text(g_tfa_key, sizeof(g_tfa_key), tfa_key);
            xSemaphoreGive(g_mutex);
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

static void run_command(cmd_t *a)
{
    if (a->op == OP_ERASE_SECRET) {
        /* serialized behind any in-flight login so its secret_save cannot
           outlive the logout wipe */
        bambu_secret_clear();
        return;
    }

    if (!bsp_wifi_connected()) {
        switch (a->op) {
        case OP_REFRESH: {
            if (generation_alive(a->generation)) {
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
                g_snapshot.device_count = 0;
                memset(g_snapshot.devices, 0, sizeof(g_snapshot.devices));
                copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                          "已登录，但打印机列表获取失败");
                xSemaphoreGive(g_mutex);
            }
            break;
        }
        case OP_SUBMIT_CODE:
            publish_state(a->generation, a->challenge,
                          a->challenge == BAMBU_CLOUD_NEED_TFA
                              ? "无法建立双重验证会话" : "验证码提交失败");
            break;
        default:
            publish_state(a->generation, BAMBU_CLOUD_FAILED,
                          "无法连接拓竹登录服务");
            break;
        }
        return;
    }

    if (a->op == OP_REFRESH) {
        bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX] = {0};
        int count = 0;
        char message[192];
        fetch_devices(a->region, a->token, devices, &count,
                      message, sizeof(message));
        if (generation_alive(a->generation)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            /* 设备列表刷新失败不会注销已经持有的云会话；保留登录态，
               让定时刷新能够恢复。message 仍会把本次失败展示给用户。 */
            g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
            g_snapshot.device_count = count;
            memcpy(g_snapshot.devices, devices, sizeof(devices));
            copy_text(g_snapshot.message, sizeof(g_snapshot.message), message);
            xSemaphoreGive(g_mutex);
        }
        return;
    }

    if (a->op == OP_PASSWORD || a->op == OP_REQUEST_EMAIL_CODE ||
        a->op == OP_REQUEST_SMS_CODE) {
        cJSON *root = cJSON_CreateObject();
        const bool password_login = a->op == OP_PASSWORD;
        const bool sms_code = a->op == OP_REQUEST_SMS_CODE;
        if (root) {
            cJSON_AddStringToObject(root, password_login ? "account" :
                                         (sms_code ? "phone" : "email"),
                                    a->account);
            cJSON_AddStringToObject(root, password_login ? "password" : "type",
                                    password_login ? a->password : "codeLogin");
        }
        char *json = root ? cJSON_PrintUnformatted(root) : NULL;
        cJSON_Delete(root);
        if (!json) {
            publish_state(a->generation, BAMBU_CLOUD_FAILED,
                          "内存不足，无法完成操作");
            return;
        }
        bambu_http_result_t r = {0};
        bool ok = bambu_http_request(api_host(a->region),
            password_login ? "/v1/user-service/user/login" :
            (sms_code ? "/v1/user-service/user/sendsmscode"
                      : "/v1/user-service/user/sendemail/code"),
            HTTP_METHOD_POST, json, NULL, NULL, NULL, &r);
        bambu_http_wipe(json, strlen(json));
        cJSON_free(json);
        if (!ok) {
            char msg[192];
            response_message(&r, "无法连接拓竹登录服务", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        } else if (password_login) {
            handle_login_reply(a, &r);
        } else if (r.status == 200) {
            publish_state(a->generation, BAMBU_CLOUD_NEED_CODE,
                          sms_code ? "验证码已发送到手机，请输入验证码"
                                   : "验证码已发送到邮箱，请输入验证码");
        } else {
            char msg[192];
            response_message(&r, "验证码发送失败", msg, sizeof(msg));
            publish_state(a->generation, BAMBU_CLOUD_FAILED, msg);
        }
        bambu_http_free(&r);
        return;
    }

    if (a->op == OP_SUBMIT_CODE) {
        bambu_http_result_t r = {0};
        if (a->challenge == BAMBU_CLOUD_NEED_CODE) {
            char *json = make_login_json(a->account, "code", a->code);
            if (!json) {
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE,
                              "内存不足，无法完成操作");
                return;
            }
            bool ok = bambu_http_request(api_host(a->region),
                "/v1/user-service/user/login", HTTP_METHOD_POST, json,
                NULL, NULL, NULL, &r);
            cJSON_free(json);
            if (!ok) {
                char msg[192];
                response_message(&r, "验证码提交失败", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            } else if (r.status == 200) {
                handle_login_reply(a, &r);
            } else {
                char msg[192];
                response_message(&r, "验证码不正确", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_CODE, msg);
            }
        } else {
            bambu_http_result_t csrf_r = {0};
            bool csrf_ok = bambu_http_request(site_host(a->region), "/api/csrf",
                                              HTTP_METHOD_GET, NULL, NULL,
                                              "", NULL, &csrf_r);
            char csrf[1024] = {0};
            bambu_http_cookie_value(csrf_r.cookies, "bbl_csrf_token",
                                    csrf, sizeof(csrf));
            if (!csrf_ok || csrf_r.status != 200 || !csrf[0]) {
                char msg[192];
                response_message(&csrf_r, "无法建立双重验证会话", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_TFA, msg);
                bambu_http_free(&csrf_r);
                bambu_http_wipe(csrf, sizeof(csrf));
                return;
            }
            cJSON *root = cJSON_CreateObject();
            if (root) {
                cJSON_AddStringToObject(root, "tfaKey", a->tfa_key);
                cJSON_AddStringToObject(root, "tfaCode", a->code);
            }
            char *json = root ? cJSON_PrintUnformatted(root) : NULL;
            cJSON_Delete(root);
            if (!json) {
                bambu_http_free(&csrf_r);
                bambu_http_wipe(csrf, sizeof(csrf));
                publish_state(a->generation, BAMBU_CLOUD_NEED_TFA,
                              "内存不足，无法完成操作");
                return;
            }
            bool ok = bambu_http_request(site_host(a->region),
                "/api/sign-in/tfa", HTTP_METHOD_POST, json, NULL,
                csrf_r.cookies, csrf, &r);
            cJSON_free(json);
            bambu_http_free(&csrf_r);
            bambu_http_wipe(csrf, sizeof(csrf));
            if (!ok || r.status != 200) {
                char msg[192];
                response_message(&r, "验证器验证码不正确", msg, sizeof(msg));
                publish_state(a->generation, BAMBU_CLOUD_NEED_TFA, msg);
            } else {
                cJSON *reply = r.body ? cJSON_Parse(r.body) : NULL;
                const char *token = reply ? extract_token(reply) : NULL;
                char token_cookie[TOKEN_MAX] = {0};
                if (!token) {
                    bambu_http_cookie_value(r.cookies, "token",
                                            token_cookie, sizeof(token_cookie));
                    token = token_cookie;
                }
                if (token && token[0]) publish_signed_in(a, token);
                else publish_state(a->generation, BAMBU_CLOUD_NEED_TFA,
                                   "验证码已通过，但登录服务没有返回令牌");
                cJSON_Delete(reply);
                bambu_http_wipe(token_cookie, sizeof(token_cookie));
            }
        }
        bambu_http_free(&r);
    }
}

static void actor_task(void *arg)
{
    (void)arg;
    cmd_t *cmd = NULL;
    while (1) {
        if (xQueueReceive(g_queue, &cmd, portMAX_DELAY) != pdTRUE || !cmd)
            continue;
        op_t op = cmd->op;
        run_command(cmd);
        bambu_http_wipe(cmd, sizeof(*cmd));
        free(cmd);
        if (op != OP_ERASE_SECRET) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_outstanding = 0;
            xSemaphoreGive(g_mutex);
        }
    }
}

/* ------------------------- public surface -------------------------- */

void bambu_rt_init(void)
{
    if (g_ready) return;
    if (!g_mutex) {
        g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_buf);
        if (!g_mutex) return;
    }
    if (!g_queue) {
        g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(cmd_t *));
        if (!g_queue) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_snapshot.state = BAMBU_CLOUD_FAILED;
            copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                      "内存不足，无法完成操作");
            xSemaphoreGive(g_mutex);
            return;
        }
    }

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_OUT;
    g_snapshot.region = BAMBU_CLOUD_REGION_CHINA;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "请输入拓竹账号");
    xSemaphoreGive(g_mutex);

    bambu_secret_t sec;
    if (bambu_secret_load(&sec)) {
        char user_id[96] = {0};
        if (sec.user_id[0]) copy_text(user_id, sizeof(user_id), sec.user_id);
        else jwt_user_id(sec.token, user_id, sizeof(user_id));
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        copy_text(g_snapshot.account, sizeof(g_snapshot.account), sec.account);
        g_snapshot.region = sec.region == BAMBU_CLOUD_REGION_CHINA
                          ? BAMBU_CLOUD_REGION_CHINA : BAMBU_CLOUD_REGION_GLOBAL;
        copy_text(g_token, sizeof(g_token), sec.token);
        copy_text(g_user_id, sizeof(g_user_id), user_id);
        g_snapshot.state = BAMBU_CLOUD_SIGNED_IN;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "已恢复保存的登录");
        xSemaphoreGive(g_mutex);
        bambu_http_wipe(user_id, sizeof(user_id));
    }
    bambu_http_wipe(&sec, sizeof(sec));

    if (xTaskCreate(actor_task, "bambu_net", ACTOR_STACK, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_snapshot.state = BAMBU_CLOUD_FAILED;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "内存不足，无法完成操作");
        xSemaphoreGive(g_mutex);
        ESP_LOGE(TAG, "actor task create failed");
        return;
    }
    g_ready = true;
}

void bambu_rt_snapshot(bambu_cloud_snapshot_t *out)
{
    if (!out) return;
    bambu_rt_init();
    if (!g_mutex) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    *out = g_snapshot;
    xSemaphoreGive(g_mutex);
}

static bool start_cmd(cmd_t *a)
{
    if (!g_ready) { free(a); return false; }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_outstanding) {
        xSemaphoreGive(g_mutex);
        free(a);
        return false;
    }
    g_outstanding = 1;
    a->generation = g_generation;
    if (a->op == OP_REFRESH) {
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "正在刷新打印机列表…");
    } else {
        g_snapshot.state = BAMBU_CLOUD_BUSY;
        copy_text(g_snapshot.message, sizeof(g_snapshot.message),
                  "正在连接拓竹云服务…");
    }
    xSemaphoreGive(g_mutex);
    if (xQueueSend(g_queue, &a, 0) != pdTRUE) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_outstanding = 0;
        xSemaphoreGive(g_mutex);
        publish_state(a->generation, BAMBU_CLOUD_FAILED, "无法启动登录任务");
        free(a);
        return false;
    }
    return true;
}

static cmd_t *new_cmd(op_t op)
{
    cmd_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->op = op;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    a->region = g_snapshot.region;
    copy_text(a->account, sizeof(a->account), g_snapshot.account);
    xSemaphoreGive(g_mutex);
    return a;
}

bool bambu_rt_set_region(bambu_cloud_region_t region)
{
    bambu_rt_init();
    if (!g_mutex) return false;
    if (region != BAMBU_CLOUD_REGION_GLOBAL && region != BAMBU_CLOUD_REGION_CHINA)
        return false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool ok = g_snapshot.state != BAMBU_CLOUD_BUSY &&
              g_snapshot.state != BAMBU_CLOUD_SIGNED_IN;
    if (ok) g_snapshot.region = region;
    xSemaphoreGive(g_mutex);
    return ok;
}

bool bambu_rt_login_password(const char *account, const char *password)
{
    bambu_rt_init();
    if (!g_ready || !account || !account[0] || !password || !password[0])
        return false;
    cmd_t *a = new_cmd(OP_PASSWORD);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), account);
    copy_text(a->password, sizeof(a->password), password);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), account);
    xSemaphoreGive(g_mutex);
    return start_cmd(a);
}

bool bambu_rt_request_email_code(const char *email)
{
    bambu_rt_init();
    if (!g_ready || !email || !email[0]) return false;
    cmd_t *a = new_cmd(OP_REQUEST_EMAIL_CODE);
    if (!a) return false;
    copy_text(a->account, sizeof(a->account), email);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), email);
    xSemaphoreGive(g_mutex);
    return start_cmd(a);
}

bool bambu_rt_request_sms_code(const char *phone)
{
    bambu_rt_init();
    if (!g_ready || !phone || !phone[0]) return false;
    cmd_t *a = new_cmd(OP_REQUEST_SMS_CODE);
    if (!a) return false;
    if (a->region != BAMBU_CLOUD_REGION_CHINA) {
        free(a);
        return false;
    }
    copy_text(a->account, sizeof(a->account), phone);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    copy_text(g_snapshot.account, sizeof(g_snapshot.account), phone);
    xSemaphoreGive(g_mutex);
    return start_cmd(a);
}

bool bambu_rt_submit_code(const char *code)
{
    bambu_rt_init();
    if (!g_ready || !code || !code[0]) return false;
    cmd_t *a = new_cmd(OP_SUBMIT_CODE);
    if (!a) return false;
    copy_text(a->code, sizeof(a->code), code);
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    a->challenge = g_snapshot.state;
    copy_text(a->tfa_key, sizeof(a->tfa_key), g_tfa_key);
    xSemaphoreGive(g_mutex);
    if (a->challenge != BAMBU_CLOUD_NEED_CODE &&
        a->challenge != BAMBU_CLOUD_NEED_TFA) {
        free(a);
        return false;
    }
    return start_cmd(a);
}

bool bambu_rt_refresh_devices(void)
{
    bambu_rt_init();
    if (!g_ready) return false;
    cmd_t *a = new_cmd(OP_REFRESH);
    if (!a) return false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bool signed_in = g_snapshot.state == BAMBU_CLOUD_SIGNED_IN;
    copy_text(a->token, sizeof(a->token), g_token);
    xSemaphoreGive(g_mutex);
    if (!signed_in || !a->token[0]) {
        free(a);
        return false;
    }
    return start_cmd(a);
}

void bambu_rt_logout(void)
{
    bambu_rt_init();
    g_generation++;

    /* NVS wipe rides the same queue so it cannot interleave with an
       in-flight login's secret_save; without a live actor (init failed)
       or a full queue the inline erase is a fast flash op. */
    cmd_t *erase = NULL;
    if (g_ready && g_queue) {
        erase = calloc(1, sizeof(*erase));
        if (erase && xQueueSend(g_queue, &erase, 0) != pdTRUE) {
            free(erase);
            erase = NULL;
        }
    }
    if (!erase) bambu_secret_clear();

    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    bambu_cloud_region_t region = g_snapshot.region;
    bambu_http_wipe(g_token, sizeof(g_token));
    bambu_http_wipe(g_user_id, sizeof(g_user_id));
    bambu_http_wipe(g_tfa_key, sizeof(g_tfa_key));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = BAMBU_CLOUD_SIGNED_OUT;
    g_snapshot.region = region;
    copy_text(g_snapshot.message, sizeof(g_snapshot.message), "请输入拓竹账号");
    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "signed out");
}
