#pragma once
/*
 * 应用配置：WiFi 凭据 + Moonraker 连接参数。
 * 文件格式为简单 key=value 行（# 注释），两端通用，不引 JSON 库：
 *   network.conf  : ssid=... / pass=...
 *   moonraker.conf: 每个打印机槽位的名称、模式、主机、端口和 API Key
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
    char     name[48];           /* 用户自定义打印机名称，可空 */
    char     host[64];
    uint16_t port;              /* 缺省 7125 */
    char     api_key[64];       /* 可空 */
    bool     valid;
} moonraker_conf_t;

/* 打印机通信生态选择。Bambu 登录配置由独立 cloud profile 管理。 */
typedef enum {
    MACHINE_MODE_KLIPPER = 0,
    MACHINE_MODE_BAMBU,
} machine_mode_t;

/* 拓竹连接方式。云端受官方权限限制，只用于状态监视；局域网开发者模式
 * 可在后端支持相应命令后开放控制。每个打印机槽位独立保存。 */
typedef enum {
    BAMBU_LINK_CLOUD_MONITOR = 0,
    BAMBU_LINK_LAN,
} bambu_link_t;

typedef struct {
    char serial[40];
    char name[64];            /* 拓竹云端设备名，本地只缓存、不提供别名编辑 */
    char model[48];
    bool valid;
} bambu_device_conf_t;

/* 多打印机：最多 6 槽。moonraker.conf 新格式：
 *   active=N
 *   machine_mode_0=klipper|bambu / name_0=... / host_0=... / port_0=7125 / api_key_0=...
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
bool settings_save_printer_name(const char *name);              /* 当前槽 */
bool settings_save_printer_name_slot(int slot, const char *name);
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
machine_mode_t settings_load_machine_mode(void);             /* 当前槽，缺省 Klipper */
bool settings_save_machine_mode(machine_mode_t mode);         /* 当前槽 */
machine_mode_t settings_load_machine_mode_slot(int slot);
bool settings_save_machine_mode_slot(int slot, machine_mode_t mode);
bambu_link_t settings_load_bambu_link(void);                  /* 当前槽，缺省云端监视 */
bool settings_save_bambu_link(bambu_link_t link);              /* 当前槽 */
bambu_link_t settings_load_bambu_link_slot(int slot);
bool settings_save_bambu_link_slot(int slot, bambu_link_t link);
bool settings_load_bambu_device(bambu_device_conf_t *out);
bool settings_save_bambu_device(const bambu_device_conf_t *in);
bool settings_load_bambu_device_slot(int slot, bambu_device_conf_t *out);
bool settings_save_bambu_device_slot(int slot, const bambu_device_conf_t *in);

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
