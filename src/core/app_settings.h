#pragma once
/*
 * 应用配置：WiFi 凭据 + Moonraker 连接参数。
 * 文件格式为简单 key=value 行（# 注释），两端通用，不引 JSON 库：
 *   network.conf  : ssid=... / pass=...
 *   moonraker.conf: host=... / port=7125 / api_key=...（可空，trusted_clients 免鉴权）
 * 存储介质由 bsp_conf 决定（esp32=LittleFS，desktop=本地文件）。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    char pass[64];
    bool valid;
} wifi_conf_t;

typedef struct {
    char     host[64];
    uint16_t port;              /* 缺省 7125 */
    char     api_key[64];       /* 可空 */
    bool     valid;
} moonraker_conf_t;

/* 多打印机：最多 6 槽。moonraker.conf 新格式：
 *   active=N
 *   host_0=... / port_0=7125 / api_key_0=...
 *   ...（host_1..host_5 同理）
 * 旧格式（host=/port=/api_key=）读取时自动迁移为槽 0。
 * settings_load/save_moonraker 操作"当前槽"，调用方无感。 */
#define PRINTER_SLOTS 6

bool settings_load_wifi(wifi_conf_t *out);
bool settings_save_wifi(const wifi_conf_t *in);

bool settings_load_moonraker(moonraker_conf_t *out);              /* 当前槽 */
bool settings_save_moonraker(const moonraker_conf_t *in);         /* 当前槽 */
bool settings_load_moonraker_slot(int slot, moonraker_conf_t *out);
bool settings_save_moonraker_slot(int slot, const moonraker_conf_t *in);
int  settings_load_active_printer(void);                          /* 0..PRINTER_SLOTS-1 */
bool settings_save_active_printer(int slot);

/* 本机偏好（klipperscreen.conf，对齐 KlipperScreen 的偏好文件习惯）。
 * 保存均为按键更新，同一文件里的其他偏好不丢。
 * language=zh|en（缺省 zh）；brightness=0-100（缺省 100）。 */
bool settings_load_language(char *out, size_t len);
bool settings_save_language(const char *lang);
int  settings_load_brightness(void);
bool settings_save_brightness(int pct);
int  settings_load_screen_off(void);      /* 自动息屏秒数，0=永不 */
bool settings_save_screen_off(int sec);

/* 显示偏好（同存 klipperscreen.conf）：
 * display_invert=0/1（反色）；display_rotate=0/1（180° 旋转）；theme=dark|light（缺省 dark） */
int  settings_load_display_invert(void);
bool settings_save_display_invert(int en);
int  settings_load_display_rotate(void);
bool settings_save_display_rotate(int en);
void settings_load_theme(char *out, size_t len);
bool settings_save_theme(const char *theme);

#ifdef __cplusplus
}
#endif
