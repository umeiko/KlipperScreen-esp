#include "moonraker_bed_level_esp32.h"
#if defined(ESP_PLATFORM) && BSP_HAS_BED_LEVEL
#include "bed_level_config.h"
#include "app_settings.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
typedef struct {
    bed_level_config_stream_t stream;
    bed_level_data_t result;
    moonraker_conf_t conf;
    atomic_bool cancelled;
} bed_job_t;
static portMUX_TYPE job_lock = portMUX_INITIALIZER_UNLOCKED;
static bed_job_t *active, *ready;
static void config_worker(void *arg)
{
    bed_job_t *job = arg;
    int64_t deadline = esp_timer_get_time() + 15000000;
    esp_http_client_config_t cfg = {
        .host = job->conf.host, .port = job->conf.port,
        .path = "/printer/objects/query?configfile=config",
        .transport_type = HTTP_TRANSPORT_OVER_TCP, .timeout_ms = 1500,
        .buffer_size = 1024, .buffer_size_tx = 512, .disable_auto_redirect = true,
    };
    bool ok = false;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http) {
        if (job->conf.api_key[0]) esp_http_client_set_header(http, "X-Api-Key", job->conf.api_key);
        if (esp_http_client_open(http, 0) == ESP_OK && esp_http_client_fetch_headers(http) >= 0 &&
            esp_http_client_get_status_code(http) == 200) {
            char chunk[512];
            while (!atomic_load(&job->cancelled) && esp_timer_get_time() < deadline) {
                int n = esp_http_client_read(http, chunk, sizeof(chunk));
                if (n <= 0 || !bed_level_config_feed(&job->stream, chunk, n)) break;
                vTaskDelay(1);
            }
            ok = !atomic_load(&job->cancelled) && esp_timer_get_time() < deadline &&
                 esp_http_client_is_complete_data_received(http) && bed_level_config_finish(&job->stream, &job->result);
        }
        esp_http_client_cleanup(http);
    }
    if (!ok) job->result.state = BED_LEVEL_FAILED;
    portENTER_CRITICAL(&job_lock);
    bool discard = atomic_load(&job->cancelled);
    active = NULL;
    if (!discard) ready = job;
    portEXIT_CRITICAL(&job_lock);
    if (discard) free(job);
    vTaskDelete(NULL);
}
bool moonraker_bed_level_start(void)
{
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 32 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 12 * 1024) return false;
    bed_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    atomic_init(&job->cancelled, false); bed_level_config_begin(&job->stream);
    if (!settings_load_moonraker(&job->conf)) { free(job); return false; }
    portENTER_CRITICAL(&job_lock);
    bool busy = active || ready;
    if (!busy) active = job;
    portEXIT_CRITICAL(&job_lock);
    if (busy) { free(job); return false; }
    if (xTaskCreate(config_worker, "bed_config", 6144, job, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&job_lock); active = NULL; portEXIT_CRITICAL(&job_lock);
        free(job); return false;
    }
    return true;
}
bool moonraker_bed_level_poll(bed_level_data_t *out)
{
    portENTER_CRITICAL(&job_lock);
    bed_job_t *job = ready; ready = NULL;
    portEXIT_CRITICAL(&job_lock);
    if (!job) return false;
    *out = job->result; free(job); return true;
}
void moonraker_bed_level_cancel(void)
{
    portENTER_CRITICAL(&job_lock);
    if (active) atomic_store(&active->cancelled, true);
    bed_job_t *job = ready; ready = NULL;
    portEXIT_CRITICAL(&job_lock);
    free(job);
}
#endif
