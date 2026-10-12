#include "bambu_secret_esp32.h"

#include "bambu_http_esp32.h"   /* bambu_http_wipe */
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "bambu_secret";

#define NVS_NAMESPACE "kr_bambu"   /* <= 15 chars */
#define NVS_KEY       "profile"

static bool str_ok(const char *s, size_t cap)
{
    /* must be NUL terminated inside the blob and non-empty */
    return memchr(s, 0, cap) != NULL && s[0] != 0;
}

bool bambu_secret_load(bambu_secret_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, NVS_KEY, NULL, &len);
    if (err == ESP_OK && len == sizeof(*out))
        err = nvs_get_blob(h, NVS_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*out) ||
        out->schema != BAMBU_SECRET_SCHEMA ||
        (out->region != 0u && out->region != 1u) ||
        !str_ok(out->account, sizeof(out->account)) ||
        !str_ok(out->token, sizeof(out->token))) {
        bambu_http_wipe(out, sizeof(*out));
        return false;
    }
    /* user_id may legitimately be empty; force termination regardless */
    out->account[sizeof(out->account) - 1] = 0;
    out->user_id[sizeof(out->user_id) - 1] = 0;
    out->token[sizeof(out->token) - 1] = 0;
    return true;
}

bool bambu_secret_save(uint32_t region, const char *account,
                       const char *user_id, const char *token)
{
    if (!account || !account[0] || !token || !token[0] ||
        strlen(token) >= BAMBU_SECRET_TOKEN_MAX) return false;
    bambu_secret_t s;
    memset(&s, 0, sizeof(s));
    s.schema = BAMBU_SECRET_SCHEMA;
    s.region = region;
    snprintf(s.account, sizeof(s.account), "%s", account);
    snprintf(s.user_id, sizeof(s.user_id), "%s", user_id ? user_id : "");
    snprintf(s.token, sizeof(s.token), "%s", token);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s, sizeof(s));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    bambu_http_wipe(&s, sizeof(s));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "profile save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

void bambu_secret_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}
