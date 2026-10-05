/* 天气获取：高德地图天气（restapi.amap.com，国内直连稳定）。
 * 优先 HTTPS（信任锚内嵌高德服务器证书），失败降级 HTTP 明文；
 * 域名解析/连接不可靠时自动切换 IP 直连（补 Host 头）。
 * 需高德开放平台免费 Web 服务 Key（后台配置）；
 * 城市用 adcode 行政区划码（后台配置，默认渭南 610500）。
 * 后台任务每 30 分钟拉一次（实时天气 + 三天预报），
 * 缓存供屏保读取；断网/失败时保持 invalid，屏保回退纯时钟。 */
#include "weather.h"
#include "app_settings.h"
#include "bsp.h"
#include "bsp_wifi.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "assets/amap_cert.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "weather"
#define W_API_HOST  "restapi.amap.com"
#define W_API_IP    "59.82.132.217"   /* 高德 GDS IP（2026-10 解析），IP 变更需更新 */
#define W_REFRESH_MS (30 * 60 * 1000)   /* 30 分钟刷新一次 */
#define W_RETRY_MS    (20 * 1000)       /* 失败快速重试间隔 */
#define W_TIMEOUT_MS 8000
#define W_BUF_SIZE  4096

static weather_data_t s_data;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;

/* use_tls: 走 HTTPS（信任锚=高德证书）；use_ip: 直连 IP（域名不可靠时） */
static int http_get(const char *url, bool use_tls, bool use_ip, char *buf, size_t bufsz)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = W_TIMEOUT_MS,
        .buffer_size = W_BUF_SIZE,
        .cert_pem = use_tls ? amap_cert_pem : NULL,   /* 高德证书信任锚 */
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
        .host = use_ip ? W_API_HOST : NULL,           /* IP 直连时补 Host 头 */
        /* 重定向默认自动跟随（http 307 → https） */
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return 0;
    esp_err_t err = esp_http_client_open(cl, 0);
    int total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        if (esp_http_client_get_status_code(cl) == 200) {
            int r;
            while (total < (int)bufsz - 1 &&
                   (r = esp_http_client_read(cl, buf + total, bufsz - 1 - (size_t)total)) > 0) {
                total += r;
            }
        }
    }
    buf[total] = 0;
    esp_http_client_cleanup(cl);
    return total;
}

/* 请求尝试链：https 域名 → http 域名 → https IP → http IP，取首个成功；每步打日志定位卡点 */
static int fetch_weather(const char *path_query, char *buf)
{
    char url[340];
    int got;
    snprintf(url, sizeof(url), "https://%s%s", W_API_HOST, path_query);
    got = http_get(url, true, false, buf, W_BUF_SIZE);
    if (got >= 16) { ESP_LOGI(TAG, "ok via https-host got=%d", got); return got; }
    ESP_LOGW(TAG, "fail https-host got=%d", got);
    snprintf(url, sizeof(url), "http://%s%s", W_API_HOST, path_query);
    got = http_get(url, false, false, buf, W_BUF_SIZE);
    if (got >= 16) { ESP_LOGI(TAG, "ok via http-host got=%d", got); return got; }
    ESP_LOGW(TAG, "fail http-host got=%d", got);
    snprintf(url, sizeof(url), "https://%s%s", W_API_IP, path_query);
    got = http_get(url, true, true, buf, W_BUF_SIZE);
    if (got >= 16) { ESP_LOGI(TAG, "ok via https-ip got=%d", got); return got; }
    ESP_LOGW(TAG, "fail https-ip got=%d", got);
    snprintf(url, sizeof(url), "http://%s%s", W_API_IP, path_query);
    got = http_get(url, false, true, buf, W_BUF_SIZE);
    if (got >= 16) { ESP_LOGI(TAG, "ok via http-ip got=%d", got); return got; }
    ESP_LOGW(TAG, "fail http-ip got=%d", got);
    return got;
}

/* 返回 JSON 里路径节点：json_get_str(root, "now", "temp", out) */
static bool json_get_str(const cJSON *root, const char *obj, const char *key, char *out, size_t outsz)
{
    if (!root) return false;
    const cJSON *o = root;
    if (obj) {
        o = cJSON_GetObjectItemCaseSensitive(root, obj);
        if (!o) return false;
    }
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || !cJSON_IsString(it) || !it->valuestring) return false;
    snprintf(out, outsz, "%s", it->valuestring);
    return true;
}

/* 高德天气文字 → 简化图标（雪>雨>雾>阴>云>晴 优先） */
static weather_icon_t icon_from_text(const char *w)
{
    if (!w || !w[0]) return WEATHER_UNKNOWN;
    if (strstr(w, "雪"))     return WEATHER_SNOW;
    if (strstr(w, "雨") || strstr(w, "雷")) return WEATHER_RAIN;
    if (strstr(w, "雾") || strstr(w, "霾")) return WEATHER_FOG;
    if (strstr(w, "阴"))     return WEATHER_OVERCAST;
    if (strstr(w, "云") || strstr(w, "间")) return WEATHER_PARTLY;
    if (strstr(w, "晴"))     return WEATHER_SUN;
    return WEATHER_UNKNOWN;
}

/* 单次完整刷新；成功返回 true。
 * 高德接口：extensions=base 返回 lives[0]（实时天气/温度），
 *          extensions=all 返回 forecasts[0].casts[]（三天预报），两者不共存，
 *          故发两次 HTTP 请求（实测设备网络 443 证书验证失败，直接用明文 HTTP）。 */
static bool fetch_all(void)
{
    char key[64], code[16], name[16];
    settings_load_weather_key(key, sizeof(key));
    settings_load_weather_city(code, sizeof(code));
    settings_load_weather_city_name(name, sizeof(name));
    if (!key[0]) {
        ESP_LOGW(TAG, "no amap key configured (屏保显示天气未配置)");
        return false;
    }
    if (!code[0]) snprintf(code, sizeof(code), "%s", "610500");   /* 缺省渭南 */
    if (!name[0]) snprintf(name, sizeof(name), "%s", "渭南");

    if (!bsp_wifi_connected()) {
        ESP_LOGW(TAG, "wifi not connected, skip");
        return false;
    }

    char *buf = malloc(W_BUF_SIZE);
    if (!buf) return false;

    weather_data_t nd;
    memset(&nd, 0, sizeof(nd));
    nd.precip = -1;
    nd.aqi = -1;
    nd.temp_min_c = -100;
    nd.temp_max_c = -100;
    snprintf(nd.city, sizeof(nd.city), "%s", name);

    char path[280];
    bool got_live = false;
    bool got_forecast = false;

    /* ---- 请求1：实时天气（extensions=base → lives[0]） ---- */
    snprintf(path, sizeof(path), "/v3/weather/weatherInfo?city=%s&key=%s&extensions=base", code, key);
    int got = fetch_weather(path, buf);
    if (got >= 16) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            char info[8] = "";
            if (json_get_str(root, NULL, "infocode", info, sizeof(info)) && strcmp(info, "10000") == 0) {
                const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "lives");
                if (a && cJSON_IsArray(a) && cJSON_GetArraySize(a) > 0) {
                    const cJSON *l = cJSON_GetArrayItem(a, 0);
                    const cJSON *t = cJSON_GetObjectItemCaseSensitive(l, "temperature");
                    const cJSON *w = cJSON_GetObjectItemCaseSensitive(l, "weather");
                    if (t && cJSON_IsString(t) && t->valuestring) {
                        nd.temp_c = atoi(t->valuestring);
                        got_live = true;
                    }
                    if (w && cJSON_IsString(w) && w->valuestring) {
                        nd.icon = (int)icon_from_text(w->valuestring);
                        snprintf(nd.category, sizeof(nd.category), "%s", w->valuestring);
                    }
                }
            } else {
                ESP_LOGW(TAG, "base infocode=%s resp=%.160s", info, buf);
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "base json parse fail: %.160s", buf);
        }
    } else {
        ESP_LOGW(TAG, "base http fail got=%d", got);
    }

    /* ---- 请求2：三天预报（extensions=all → forecasts[0].casts[0..2]） ---- */
    snprintf(path, sizeof(path), "/v3/weather/weatherInfo?city=%s&key=%s&extensions=all", code, key);
    got = fetch_weather(path, buf);
    if (got >= 16) {
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            char info[8] = "";
            if (json_get_str(root, NULL, "infocode", info, sizeof(info)) && strcmp(info, "10000") == 0) {
                const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, "forecasts");
                if (f && cJSON_IsArray(f) && cJSON_GetArraySize(f) > 0) {
                    const cJSON *c0 = cJSON_GetArrayItem(f, 0);
                    const cJSON *cs = cJSON_GetObjectItemCaseSensitive(c0, "casts");
                    if (cs && cJSON_IsArray(cs)) {
                        int n = cJSON_GetArraySize(cs);
                        if (n > 3) n = 3;
                        for (int i = 0; i < n; i++) {
                            const cJSON *it = cJSON_GetArrayItem(cs, i);
                            const cJSON *dw = cJSON_GetObjectItemCaseSensitive(it, "dayweather");
                            const cJSON *dt = cJSON_GetObjectItemCaseSensitive(it, "daytemp");
                            const cJSON *nt = cJSON_GetObjectItemCaseSensitive(it, "nighttemp");
                            if (dw && cJSON_IsString(dw) && dw->valuestring) {
                                snprintf(nd.forecast[i].text, sizeof(nd.forecast[i].text), "%s", dw->valuestring);
                                nd.forecast[i].icon = (int)icon_from_text(dw->valuestring);
                            }
                            if (dt && cJSON_IsString(dt) && dt->valuestring) nd.forecast[i].tmax = atoi(dt->valuestring);
                            if (nt && cJSON_IsString(nt) && nt->valuestring) nd.forecast[i].tmin = atoi(nt->valuestring);
                            nd.forecast_count++;
                        }
                        got_forecast = true;
                        /* 今日实时高低温以预报首日为参考 */
                        if (nd.forecast_count > 0) {
                            nd.temp_min_c = nd.forecast[0].tmin;
                            nd.temp_max_c = nd.forecast[0].tmax;
                        }
                    }
                }
            } else {
                ESP_LOGW(TAG, "all infocode=%s resp=%.160s", info, buf);
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "all json parse fail: %.160s", buf);
        }
    } else {
        ESP_LOGW(TAG, "all http fail got=%d", got);
    }

    free(buf);

    if (!got_live) {
        ESP_LOGW(TAG, "no lives data (got_live=%d got_fc=%d)", got_live, got_forecast);
        return false;
    }
    nd.valid = true;

    nd.updated_ms = (uint64_t)esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    s_data = nd;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "weather ok: %s %dC %s range=%d/%d fc=%d",
             nd.city, nd.temp_c, nd.category, nd.temp_min_c, nd.temp_max_c, nd.forecast_count);
    return true;
}

static void weather_task(void *arg)
{
    (void)arg;
    int fail_cnt = 0;
    for (;;) {
        if (fetch_all()) {
            fail_cnt = 0;
            vTaskDelay(pdMS_TO_TICKS(W_REFRESH_MS));
        } else {
            fail_cnt++;
            ESP_LOGW(TAG, "fetch failed (cnt=%d), retry in %dms", fail_cnt, W_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(W_RETRY_MS));
        }
    }
}

void weather_start(void)
{
    if (s_task) return;
    if (xTaskCreate(weather_task, "weather", 16384, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

bool weather_get(weather_data_t *out)
{
    bool v;
    portENTER_CRITICAL(&s_mux);
    *out = s_data;
    v = s_data.valid;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/* 兼容：0..6 已是 weather_icon_t 枚举，透传；其余（旧 WMO 码）归并 */
weather_icon_t weather_icon_from_code(int code)
{
    if (code >= WEATHER_SUN && code <= WEATHER_UNKNOWN) return (weather_icon_t)code;
    if (code == 0)                            return WEATHER_SUN;
    if (code == 1 || code == 2)               return WEATHER_PARTLY;
    if (code == 3)                            return WEATHER_OVERCAST;
    if (code == 45 || code == 48)             return WEATHER_FOG;
    if (code >= 51 && code <= 67)             return WEATHER_RAIN;
    if (code >= 71 && code <= 77)             return WEATHER_SNOW;
    if (code >= 80 && code <= 82)             return WEATHER_RAIN;
    if (code == 85 || code == 86)             return WEATHER_SNOW;
    if (code >= 95 && code <= 99)             return WEATHER_RAIN;
    return WEATHER_UNKNOWN;
}
