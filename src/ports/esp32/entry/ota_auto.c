/*
 * OTA 自动检测升级：开机连上 WiFi 后检查配置的固件服务器，
 * 有新版固件自动下载升级到另一槽位并重启。
 *
 * 服务器目录约定（ota_url 指向该目录，末尾可带/可无/）：
 *   version.txt  一行版本串（任意字符串，发布新固件时更新）
 *   firmware.bin 完整 app 固件（.bin）
 *
 * 防循环：升级成功后记录已应用版本串（ota_applied），
 * version.txt 不变时开机跳过；只有发布方更新 version.txt 才会再次触发。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "app_settings.h"
#include "bsp_wifi.h"

#define TAG "ota_auto"

#define OTA_WAIT_WIFI_MS   (30 * 1000)
#define OTA_POLL_MS        500
#define OTA_HTTP_TIMEOUT_MS 10000
#define OTA_BUF_SIZE       4096
#define OTA_REBOOT_DELAY_MS 1500

/* 小文件 GET（version.txt），返回读到的字节数 */
static int http_get_small(const char *url, char *buf, size_t bufsz)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_BUF_SIZE,
        .keep_alive_enable = false,
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

/* 流式下载固件并写入 OTA 分区。成功返回 0，失败返回 -1 */
static int ota_download_firmware(const char *url)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA partition");
        return -1;
    }
    ESP_LOGI(TAG, "OTA target %s @0x%x", part->label, (unsigned)part->address);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_BUF_SIZE,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { ESP_LOGE(TAG, "http init failed"); return -1; }

    esp_err_t err = esp_http_client_open(cl, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cl);
        return -1;
    }
    esp_http_client_fetch_headers(cl);
    if (esp_http_client_get_status_code(cl) != 200) {
        ESP_LOGE(TAG, "http status %d", esp_http_client_get_status_code(cl));
        esp_http_client_cleanup(cl);
        return -1;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cl);
        return -1;
    }

    char buf[OTA_BUF_SIZE];
    int total = 0, r;
    int ret = -1;
    while ((r = esp_http_client_read(cl, buf, sizeof(buf))) > 0) {
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed");
            esp_ota_abort(ota);
            goto out;
        }
        total += r;
    }
    if (r < 0) {
        ESP_LOGE(TAG, "download interrupted after %d bytes", total);
        esp_ota_abort(ota);
        goto out;
    }
    ESP_LOGI(TAG, "downloaded %d bytes", total);

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota end (invalid image): %s", esp_err_to_name(err));
        goto out;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot: %s", esp_err_to_name(err));
        goto out;
    }
    ESP_LOGI(TAG, "OTA success, will reboot to %s", part->label);
    ret = 0;

out:
    esp_http_client_cleanup(cl);
    return ret;
}

static void ota_check_task(void *arg)
{
    (void)arg;

    /* 等待 WiFi 连上（STA 在 app_main 里发起，通常几秒） */
    TickType_t deadline = xTaskGetTickCount() + OTA_WAIT_WIFI_MS / portTICK_PERIOD_MS;
    while (xTaskGetTickCount() < deadline) {
        if (bsp_wifi_connected()) break;
        vTaskDelay(OTA_POLL_MS / portTICK_PERIOD_MS);
    }
    if (!bsp_wifi_connected()) {
        ESP_LOGW(TAG, "wifi not connected, skip OTA check");
        vTaskDelete(NULL);
        return;
    }

    char base[160];
    settings_load_ota_url(base, sizeof(base));
    if (!base[0]) {
        ESP_LOGI(TAG, "no ota_url configured, skip");
        vTaskDelete(NULL);
        return;
    }

    /* 已应用过同一版本则跳过（防 version.txt 未更新导致的循环） */
    char applied[64];
    settings_load_ota_applied(applied, sizeof(applied));

    char url[320];
    if (base[strlen(base) - 1] == '/')
        snprintf(url, sizeof(url), "%sversion.txt", base);
    else
        snprintf(url, sizeof(url), "%s/version.txt", base);

    char remote[64];
    int got = http_get_small(url, remote, sizeof(remote));
    if (got <= 0) {
        ESP_LOGW(TAG, "version.txt fetch failed (%d)", got);
        vTaskDelete(NULL);
        return;
    }
    /* 去掉尾部空白/换行 */
    char *e = remote + strlen(remote);
    while (e > remote && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;

    if (applied[0] && strcmp(applied, remote) == 0) {
        ESP_LOGI(TAG, "version %s already applied, skip", remote);
        vTaskDelete(NULL);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    const char *cur = (app && app->version[0]) ? app->version : "?";
    if (strcmp(cur, remote) == 0) {
        ESP_LOGI(TAG, "already on version %s", remote);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "new version %s (current %s), downloading firmware...", remote, cur);

    if (base[strlen(base) - 1] == '/')
        snprintf(url, sizeof(url), "%sfirmware.bin", base);
    else
        snprintf(url, sizeof(url), "%s/firmware.bin", base);

    if (ota_download_firmware(url) != 0) {
        ESP_LOGE(TAG, "auto OTA failed, keep current firmware");
        vTaskDelete(NULL);
        return;
    }

    settings_save_ota_applied(remote);
    ESP_LOGW(TAG, "auto OTA done, rebooting in %d ms", OTA_REBOOT_DELAY_MS);
    vTaskDelay(OTA_REBOOT_DELAY_MS / portTICK_PERIOD_MS);
    esp_restart();
    vTaskDelete(NULL);
}

void ota_auto_start(void)
{
    BaseType_t ok = xTaskCreate(ota_check_task, "ota_auto", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    } else {
        ESP_LOGI(TAG, "auto OTA check started");
    }
}
