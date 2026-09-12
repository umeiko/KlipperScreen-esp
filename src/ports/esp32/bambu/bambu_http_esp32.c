#include "bambu_http_esp32.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "bambu_http";

#define BODY_START   2048
#define READ_CHUNK   1024
#define URL_MAX      192
#define TIMEOUT_MS   15000

typedef struct {
    char *jar;
    size_t jar_cap;
} header_ctx_t;

/* Non-optimizable explicit zeroing for secret-bearing buffers. */
void bambu_http_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

void bambu_http_cookie_put(char *jar, size_t cap, const char *set_cookie)
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

bool bambu_http_cookie_value(const char *jar, const char *name,
                             char *out, size_t cap)
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

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Set-Cookie") == 0) {
        header_ctx_t *ctx = evt->user_data;
        bambu_http_cookie_put(ctx->jar, ctx->jar_cap, evt->header_value);
    }
    return ESP_OK;
}

static char *body_alloc(size_t n)
{
#if CONFIG_SPIRAM
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

/* Grow by allocating a fresh buffer: heap_caps_realloc cannot move a block
 * between internal RAM and PSRAM, so copy-and-free stays correct for both. */
static char *body_grow(char *old, size_t old_cap, size_t new_cap)
{
    char *next = body_alloc(new_cap);
    if (!next) return NULL;
    memcpy(next, old, old_cap);
    free(old);
    return next;
}

bool bambu_http_request(const char *host, const char *path, int method,
                        const char *json, const char *bearer,
                        const char *cookie_jar, const char *csrf,
                        bambu_http_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->esp_err = ESP_FAIL;
    if (cookie_jar)
        snprintf(out->cookies, sizeof(out->cookies), "%s", cookie_jar);

    char url[URL_MAX];
    if (snprintf(url, sizeof(url), "https://%s%s", host, path) >= (int)sizeof(url)) {
        out->esp_err = ESP_ERR_INVALID_ARG;
        return false;
    }

    header_ctx_t hctx = { out->cookies, sizeof(out->cookies) };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = (esp_http_client_method_t)method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = &hctx,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        /* hostname verification stays enabled (no skip_cert_common_name_check) */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        out->esp_err = ESP_ERR_NO_MEM;
        return false;
    }

    bool ok = false;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent",
                               "bambu_network_agent/01.09.05.01");
    if (bearer && bearer[0]) {
        char auth[2100];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
        bambu_http_wipe(auth, sizeof(auth));
    }
    if (cookie_jar && cookie_jar[0])
        esp_http_client_set_header(client, "Cookie", cookie_jar);
    if (csrf && csrf[0])
        esp_http_client_set_header(client, "x-bbl-csrf-token", csrf);

    size_t body_len = json ? strlen(json) : 0;
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) { out->esp_err = err; goto done; }
    if (body_len) {
        int written = esp_http_client_write(client, json, (int)body_len);
        if (written < 0) { out->esp_err = ESP_FAIL; goto done; }
    }
    int64_t hdr_len = esp_http_client_fetch_headers(client);
    if (hdr_len < 0) {
        out->esp_err = esp_http_client_get_errno(client);
        goto done;
    }
    out->status = esp_http_client_get_status_code(client);

    size_t used = 0, cap = BODY_START;
    out->body = body_alloc(cap);
    if (!out->body) { out->esp_err = ESP_ERR_NO_MEM; goto done; }
    char chunk[READ_CHUNK];
    while (1) {
        int got = esp_http_client_read(client, chunk, sizeof(chunk));
        if (got < 0) { out->esp_err = ESP_FAIL; goto done; }
        if (got == 0) break;
        if (used + (size_t)got + 1 > BAMBU_HTTP_BODY_MAX) {
            ESP_LOGW(TAG, "response body over %u bytes, dropping",
                     (unsigned)BAMBU_HTTP_BODY_MAX);
            out->esp_err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        if (used + (size_t)got + 1 > cap) {
            size_t next = cap;
            while (next < used + (size_t)got + 1) next *= 2;
            char *grown = body_grow(out->body, cap, next);
            if (!grown) { out->esp_err = ESP_ERR_NO_MEM; goto done; }
            out->body = grown;
            cap = next;
        }
        memcpy(out->body + used, chunk, (size_t)got);
        used += (size_t)got;
    }
    out->body[used] = 0;
    out->esp_err = ESP_OK;
    ok = true;

done:
    if (!ok) {
        free(out->body);
        out->body = NULL;
    }
    esp_http_client_cleanup(client);
    return ok;
}

void bambu_http_free(bambu_http_result_t *r)
{
    free(r->body);
    r->body = NULL;
}
