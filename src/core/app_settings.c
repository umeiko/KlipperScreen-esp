/*
 * 应用配置读写：key=value 行格式，见 app_settings.h
 */
#include "app_settings.h"
#include "bsp_conf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CONF_BUF_SIZE 2048   /* moonraker.conf 要装 6 槽 × (host+port+key) */

static bool conf_update_key(const char *file, const char *key, const char *val);

/* 在 buf（key=value 行集合）里找 key，把值拷到 out。返回是否命中 */
static bool kv_get(const char *buf, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len && p[0] != '#' && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = line_len - klen - 1;
            const char *v = p + klen + 1;
            /* 去掉行尾 \r */
            while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;
            if (vlen >= out_len) vlen = out_len - 1;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return true;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

/* 读整个配置文件到堆缓冲区（调用方负责 free）；失败返回 NULL。
 * 缓冲区 2KB 不能放栈上：main 任务栈小，栈上分配曾导致启动栈溢出。 */
static char *conf_load(const char *file)
{
    char *buf = malloc(CONF_BUF_SIZE);
    if (!buf) return NULL;
    if (bsp_conf_read(file, buf, CONF_BUF_SIZE) < 0) { free(buf); return NULL; }
    return buf;
}

bool settings_load_wifi(wifi_conf_t *out)
{
    memset(out, 0, sizeof(*out));
    char *buf = conf_load("network.conf");
    if (!buf) return false;
    bool ok = kv_get(buf, "ssid", out->ssid, sizeof(out->ssid)) && out->ssid[0];
    if (ok) {
        kv_get(buf, "pass", out->pass, sizeof(out->pass));   /* 开放网络可空 */
        out->valid = true;
    }
    free(buf);
    return ok;
}

bool settings_save_wifi(const wifi_conf_t *in)
{
    char buf[160];   /* ssid(33)+pass(64) 富余，无需大缓冲 */
    snprintf(buf, sizeof(buf), "# WiFi credentials\nssid=%s\npass=%s\n", in->ssid, in->pass);
    return bsp_conf_write("network.conf", buf) == 0;
}

/* 读单槽：新格式 host_N/port_N/api_key_N；槽 0 兼容旧格式 host=/port=/api_key= */
bool settings_load_moonraker_slot(int slot, moonraker_conf_t *out)
{
    char key[16];
    memset(out, 0, sizeof(*out));
    out->port = 7125;
    if (slot < 0 || slot >= PRINTER_SLOTS) return false;
    char *buf = conf_load("moonraker.conf");
    if (!buf) return false;

    snprintf(key, sizeof(key), "host_%d", slot);
    bool has = kv_get(buf, key, out->host, sizeof(out->host));
    if (!has && slot == 0)   /* 旧格式迁移：裸 host= 视为槽 0 */
        has = kv_get(buf, "host", out->host, sizeof(out->host));
    if (!has || !out->host[0]) { free(buf); return false; }

    snprintf(key, sizeof(key), "port_%d", slot);
    char port[8];
    if (!kv_get(buf, key, port, sizeof(port)) && slot == 0)
        kv_get(buf, "port", port, sizeof(port));
    if (port[0]) {
        int p = atoi(port);
        if (p > 0 && p < 65536) out->port = (uint16_t)p;
    }
    snprintf(key, sizeof(key), "api_key_%d", slot);
    if (!kv_get(buf, key, out->api_key, sizeof(out->api_key)) && slot == 0)
        kv_get(buf, "api_key", out->api_key, sizeof(out->api_key));
    free(buf);
    out->valid = true;
    return true;
}

int settings_load_active_printer(void)
{
    char val[8];
    char *buf = conf_load("moonraker.conf");
    if (!buf) return 0;
    bool has = kv_get(buf, "active", val, sizeof(val));
    free(buf);
    if (!has) return 0;
    int s = atoi(val);
    return (s >= 0 && s < PRINTER_SLOTS) ? s : 0;
}

bool settings_save_active_printer(int slot)
{
    char val[8];
    if (slot < 0 || slot >= PRINTER_SLOTS) return false;
    snprintf(val, sizeof(val), "%d", slot);
    return conf_update_key("moonraker.conf", "active", val);
}

bool settings_load_moonraker(moonraker_conf_t *out)
{
    return settings_load_moonraker_slot(settings_load_active_printer(), out);
}

/* 写单槽：读出全部槽→改目标槽→按新格式整体重写（顺带完成旧格式迁移） */
bool settings_save_moonraker_slot(int slot, const moonraker_conf_t *in)
{
    if (slot < 0 || slot >= PRINTER_SLOTS) return false;
    moonraker_conf_t all[PRINTER_SLOTS];
    for (int i = 0; i < PRINTER_SLOTS; i++)
        settings_load_moonraker_slot(i, &all[i]);
    all[slot] = *in;

    char *buf = malloc(CONF_BUF_SIZE);
    if (!buf) return false;
    size_t n = snprintf(buf, CONF_BUF_SIZE, "# Moonraker printers\nactive=%d\n",
                        settings_load_active_printer());
    for (int i = 0; i < PRINTER_SLOTS; i++) {
        if (!all[i].host[0]) continue;
        n += snprintf(buf + n, CONF_BUF_SIZE - n, "host_%d=%s\nport_%d=%u\napi_key_%d=%s\n",
                      i, all[i].host, i, (unsigned)(all[i].port ? all[i].port : 7125),
                      i, all[i].api_key);
    }
    bool ok = bsp_conf_write("moonraker.conf", buf) == 0;
    free(buf);
    return ok;
}

bool settings_save_moonraker(const moonraker_conf_t *in)
{
    return settings_save_moonraker_slot(settings_load_active_printer(), in);
}

/* 读文件→替换/追加 key 行→整体写回（保留文件里其他 key，供 klipperscreen.conf
   这种多偏好共存的文件使用；缓冲区大，走堆，不占调用方栈） */
static bool conf_update_key(const char *file, const char *key, const char *val)
{
    char *buf = malloc(CONF_BUF_SIZE), *out = malloc(CONF_BUF_SIZE);
    if (!buf || !out) { free(buf); free(out); return false; }
    int n = bsp_conf_read(file, buf, CONF_BUF_SIZE);
    if (n < 0) buf[0] = 0;

    size_t olen = 0, klen = strlen(key);
    const char *p = buf;
    bool done = false;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (!done && line_len && p[0] != '#' && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            olen += snprintf(out + olen, CONF_BUF_SIZE - olen, "%s=%s\n", key, val);
            done = true;
        } else if (line_len) {
            olen += snprintf(out + olen, CONF_BUF_SIZE - olen, "%.*s\n", (int)line_len, p);
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (!done)
        olen += snprintf(out + olen, CONF_BUF_SIZE - olen, "%s=%s\n", key, val);
    bool ok = bsp_conf_write(file, out) == 0;
    free(buf);
    free(out);
    return ok;
}

bool settings_load_language(char *out, size_t len)
{
    if (len) { out[0] = 0; strncat(out, "zh", len - 1); }   /* 缺省中文 */
    char *buf = conf_load("klipperscreen.conf");
    if (!buf) return false;
    bool ok = kv_get(buf, "language", out, len);
    free(buf);
    return ok;
}

bool settings_save_language(const char *lang)
{
    return conf_update_key("klipperscreen.conf", "language", lang);
}

int settings_load_brightness(void)
{
    char val[8];
    char *buf = conf_load("klipperscreen.conf");
    if (!buf) return 100;
    bool got = kv_get(buf, "brightness", val, sizeof(val));
    free(buf);
    if (!got) return 100;
    int pct = atoi(val);
    return (pct >= 0 && pct <= 100) ? pct : 100;
}

bool settings_save_brightness(int pct)
{
    char val[8];
    snprintf(val, sizeof(val), "%d", pct);
    return conf_update_key("klipperscreen.conf", "brightness", val);
}

/* 自动息屏：秒，0=永不，缺省 0 */
int settings_load_screen_off(void)
{
    char val[8];
    char *buf = conf_load("klipperscreen.conf");
    if (!buf) return 0;
    bool got = kv_get(buf, "screen_off", val, sizeof(val));
    free(buf);
    if (!got) return 0;
    int sec = atoi(val);
    return sec >= 0 ? sec : 0;
}

bool settings_save_screen_off(int sec)
{
    char val[8];
    snprintf(val, sizeof(val), "%d", sec);
    return conf_update_key("klipperscreen.conf", "screen_off", val);
}

/* klipperscreen.conf 里的 int 偏好通用读取（缺省 def） */
static int ksc_load_int(const char *key, int def)
{
    char val[12];
    char *buf = conf_load("klipperscreen.conf");
    if (!buf) return def;
    bool got = kv_get(buf, key, val, sizeof(val));
    free(buf);
    return got ? atoi(val) : def;
}

static bool ksc_save_int(const char *key, int v)
{
    char val[12];
    snprintf(val, sizeof(val), "%d", v);
    return conf_update_key("klipperscreen.conf", key, val);
}

int  settings_load_display_invert(void)      { return ksc_load_int("display_invert", 0) != 0; }
bool settings_save_display_invert(int en)    { return ksc_save_int("display_invert", en ? 1 : 0); }
int  settings_load_display_rotate(void)      { return ksc_load_int("display_rotate", 0) != 0; }
bool settings_save_display_rotate(int en)    { return ksc_save_int("display_rotate", en ? 1 : 0); }

void settings_load_theme(char *out, size_t len)
{
    if (len) { out[0] = 0; strncat(out, "dark", len - 1); }   /* 缺省深色 */
    char *buf = conf_load("klipperscreen.conf");
    if (!buf) return;
    kv_get(buf, "theme", out, len);
    free(buf);
}

bool settings_save_theme(const char *theme)
{
    return conf_update_key("klipperscreen.conf", "theme", theme);
}
