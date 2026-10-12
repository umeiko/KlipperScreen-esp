#pragma once

/*
 * ESP32 bounded HTTPS helper for Bambu Cloud (bambu_net actor only).
 *
 * - esp_http_client + esp_crt_bundle_attach: certificate chain and hostname
 *   are verified; no insecure / skip-common-name options anywhere.
 * - Response bodies are collected in chunks with a hard 64 KiB cap, PSRAM
 *   preferred when available.
 * - Set-Cookie headers are folded into a "a=b; c=d" jar so the TFA flow can
 *   echo them back, mirroring the Windows port.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define BAMBU_HTTP_BODY_MAX   (64 * 1024)
#define BAMBU_HTTP_COOKIE_MAX 4096

typedef struct {
    int status;                        /* HTTP status, 0 when transport failed */
    char *body;                        /* heap buffer, free with bambu_http_free */
    esp_err_t esp_err;                 /* last esp_http_client error */
} bambu_http_result_t;

/* Keep per-request results safe to place on the network actor's stack.  Large
 * optional data (currently only the TFA cookie jar) is caller-owned. */
_Static_assert(sizeof(bambu_http_result_t) <= 16,
               "bambu_http_result_t must stay a small stack value");

/* method is esp_http_client's enum value passed as int (HTTP_METHOD_GET/POST).
 * json may be NULL for bodyless requests.  bearer/cookie_jar/csrf are
 * optional request header values; pass NULL to omit.  response_cookies is an
 * optional caller-owned jar updated from Set-Cookie response headers.  It may
 * alias cookie_jar: request headers are copied by esp_http_client before any
 * response event can update the jar. */
bool bambu_http_request(const char *host, const char *path, int method,
                        const char *json, const char *bearer,
                        const char *cookie_jar, const char *csrf,
                        char *response_cookies, size_t response_cookies_cap,
                        bambu_http_result_t *out);

void bambu_http_free(bambu_http_result_t *r);

/* Non-optimizable explicit zeroing for secret-bearing buffers. */
void bambu_http_wipe(void *p, size_t n);

/* Cookie jar helpers (pure C, shared semantics with the Windows port). */
void bambu_http_cookie_put(char *jar, size_t cap, const char *set_cookie);
bool bambu_http_cookie_value(const char *jar, const char *name,
                             char *out, size_t cap);
