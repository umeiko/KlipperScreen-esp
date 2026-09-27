/* File browsing uses a separate bounded HTTP stream, never the WebSocket's
 * full-message buffer/DOM/serialized-copy/LVGL reparse path. The worker never
 * takes the LVGL mutex; completion is polled without allocating an async timer. */
#include "moonraker_files_esp32.h"
#include "file_list_stream.h"
#include "app_settings.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    file_list_stream_t stream;
    moonraker_conf_t conf;
    atomic_bool cancelled;
} file_job_t;
static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static file_job_t *active, *ready;

static void files_worker(void *arg)
{
    file_job_t *job = arg;
    bool ok = false;
    int64_t deadline = esp_timer_get_time() + 20000000;
    esp_http_client_config_t cfg = {
        .host = job->conf.host, .port = job->conf.port,
        .path = "/server/files/list?root=gcodes",
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .timeout_ms = 1500, .buffer_size = 1024, .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http) {
        if (job->conf.api_key[0]) esp_http_client_set_header(http, "X-Api-Key", job->conf.api_key);
        if (esp_http_client_open(http, 0) == ESP_OK &&
            esp_http_client_fetch_headers(http) >= 0 && esp_http_client_get_status_code(http) == 200) {
            char chunk[512];
            while (!atomic_load(&job->cancelled) && esp_timer_get_time() < deadline) {
                int n = esp_http_client_read(http, chunk, sizeof(chunk));
                if (n <= 0) break;
                if (!file_list_stream_feed(&job->stream, chunk, n)) break;
                vTaskDelay(1); /* let idle/Wi-Fi run even on a fast LAN */
            }
            ok = !atomic_load(&job->cancelled) && esp_timer_get_time() < deadline &&
                 esp_http_client_is_complete_data_received(http) && file_list_stream_finish(&job->stream);
        }
        esp_http_client_cleanup(http);
    }
    if (!ok) job->stream.page.count = -1;
    ESP_LOGI("files", "page offset=%u count=%d total=%u bytes=%u heap=%u",
        job->stream.offset, job->stream.page.count, job->stream.page.total,
        (unsigned)job->stream.bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&job_lock);
    bool discard = atomic_load(&job->cancelled);
    active = NULL;
    if (!discard) ready = job;
    portEXIT_CRITICAL(&job_lock);
    if (discard) free(job);
    vTaskDelete(NULL);
}

bool moonraker_files_start(unsigned offset)
{
    /* Keep headroom for existing UI, Wi-Fi, status updates and failure text. */
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 24 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8 * 1024) return false;
    file_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    atomic_init(&job->cancelled, false);
    file_list_stream_init(&job->stream, offset);
    if (!settings_load_moonraker(&job->conf)) { free(job); return false; }
    portENTER_CRITICAL(&job_lock);
    bool busy = active != NULL || ready != NULL;
    if (!busy) active = job;
    portEXIT_CRITICAL(&job_lock);
    if (busy) { free(job); return false; }
    if (xTaskCreate(files_worker, "files_http", 6144, job, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock);
        active = NULL;
        portEXIT_CRITICAL(&job_lock);
        free(job);
        return false;
    }
    return true;
}

bool moonraker_files_poll(printer_file_page_t *page)
{
    portENTER_CRITICAL(&job_lock);
    file_job_t *job = ready;
    ready = NULL;
    portEXIT_CRITICAL(&job_lock);
    if (!job) return false;
    *page = job->stream.page;
    free(job);
    return true;
}

void moonraker_files_cancel(void)
{
    portENTER_CRITICAL(&job_lock);
    if (active) atomic_store(&active->cancelled, true);
    file_job_t *job = ready;
    ready = NULL;
    portEXIT_CRITICAL(&job_lock);
    free(job);
}
