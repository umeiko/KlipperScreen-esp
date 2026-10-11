#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "app_settings.h"
#include "bsp.h"
#include "bsp_screen_power.h"
#include "bsp_wifi.h"
#include "moonraker_client.h"
#include "printer.h"
#include "ui_app.h"
}

#include "bsp_wio.h"

extern "C" void printer_wio_debug_status(void);

static uint32_t last_tick;
static char cli_line[112];
static size_t cli_len;
static wifi_conf_t startup_wifi;
static bool startup_wifi_scan_pending;
static bool startup_wifi_connect_pending;
static uint32_t startup_wifi_connect_at;
static bsp_wifi_ap_t startup_aps[16];

static void startup_wifi_poll(void)
{
    if (startup_wifi_connect_pending) {
        if ((int32_t)(millis() - startup_wifi_connect_at) < 0) return;
        startup_wifi_connect_pending = false;
        Serial.println("WiFi: startup scan settled, connecting");
        bsp_wifi_connect(startup_wifi.ssid,
                         startup_wifi.pass[0] ? startup_wifi.pass : nullptr);
        return;
    }
    if (!startup_wifi_scan_pending) return;
    int count = bsp_wifi_scan_poll(startup_aps, 16);
    if (count == BSP_WIFI_SCAN_RUNNING) return;

    startup_wifi_scan_pending = false;
    bool visible = false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(startup_aps[i].ssid, startup_wifi.ssid) == 0) {
            visible = true;
            break;
        }
    }
    Serial.print("WiFi: startup scan ");
    Serial.println(visible ? "found saved AP" : "did not find saved AP");
    /* scanDelete() returns before all RTL8720DN scan work has settled. A human
       entering a password naturally leaves this gap; boot auto-connect must
       create it explicitly or the following join is often rejected. */
    startup_wifi_connect_at = millis() + 3000;
    startup_wifi_connect_pending = true;
}

static void cli_run(char *line)
{
    while (*line == ' ') ++line;
    if (strcmp(line, "status") == 0) {
        char ssid[BSP_WIFI_SSID_MAX + 1], ip[16];
        bool connected = bsp_wifi_current(ssid, sizeof(ssid), ip, sizeof(ip));
        Serial.print("WiFi: state=");
        Serial.print((int)bsp_wifi_status());
        Serial.print(" ssid=");
        Serial.print(connected ? ssid : "-");
        Serial.print(" ip=");
        Serial.println(connected ? ip : "-");
        printer_wio_debug_status();
        return;
    }

    if (strncmp(line, "wifi ", 5) == 0) {
        char *ssid = line + 5;
        while (*ssid == ' ') ++ssid;
        char *pass = strchr(ssid, ' ');
        if (!*ssid || !pass) {
            Serial.println("Usage: wifi <ssid> <password>");
            return;
        }
        *pass++ = 0;
        while (*pass == ' ') ++pass;
        if (!*pass || strlen(ssid) > BSP_WIFI_SSID_MAX ||
            strlen(pass) > BSP_WIFI_PASS_MAX) {
            Serial.println("WiFi: invalid SSID/password length");
            return;
        }

        wifi_conf_t wifi = {};
        snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", ssid);
        snprintf(wifi.pass, sizeof(wifi.pass), "%s", pass);
        wifi.valid = true;
        if (!settings_save_wifi(&wifi)) {
            Serial.println("WiFi: unable to save credentials");
            return;
        }
        Serial.print("WiFi: saved, connecting to ");
        Serial.println(wifi.ssid);       /* Never echo the password. */
        startup_wifi_scan_pending = false;
        startup_wifi_connect_pending = false;
        bsp_wifi_init();
        bsp_wifi_connect(wifi.ssid, wifi.pass);
        return;
    }

    if (*line) Serial.println("Commands: status | wifi <ssid> <password>");
}

static void cli_poll(void)
{
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (cli_len) {
                cli_line[cli_len] = 0;
                cli_run(cli_line);
                cli_len = 0;
            }
        } else if (ch == 8 || ch == 127) {
            if (cli_len) --cli_len;
        } else if (ch >= 32 && ch < 127 && cli_len + 1 < sizeof(cli_line)) {
            cli_line[cli_len++] = (char)ch;
        }
    }
}

/* Seeed's old SAMD core predates C++14 sized delete; GCC 9 emits it. */
void operator delete(void *ptr, size_t) noexcept { free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { free(ptr); }

void setup()
{
    Serial.begin(115200);
    uint32_t wait_start = millis();
    while (!Serial && millis() - wait_start < 1200) delay(10);
    Serial.println("\nKlipperScreen-esp / Wio Terminal");

    bsp_init();
    Serial.println("BOOT: bsp ready");
    settings_seed_defaults();
    Serial.println("BOOT: settings ready");
    bsp_input_init();
    bsp_disp_set_invert(settings_load_display_invert());
    bsp_disp_set_rotate180(settings_load_display_rotate());
    bsp_disp_set_mirror_x(settings_load_display_mirror());
    ui_app_create();
    Serial.println("BOOT: UI tree ready");
    bsp_set_brightness(settings_load_brightness());
    bsp_set_screen_timeout(settings_load_screen_off());

    Serial.println("BOOT: WiFi init");
    bsp_wifi_init();
    if (settings_load_wifi(&startup_wifi)) {
        /* Seeed's RTL8720DN firmware is unreliable when wifi_connect follows
           a coprocessor reset immediately.  The same credentials work after
           the WiFi panel has scanned once, so warm the radio asynchronously
           with that exact sequence before boot auto-connect. */
        startup_wifi_scan_pending = true;
        bsp_wifi_scan_start();
    }

    last_tick = millis();
    Serial.println("UI ready: five-way=move/OK, top C=Back");
}

void loop()
{
    uint32_t now = millis();
    uint32_t elapsed = now - last_tick;
    if (elapsed) {
        lv_tick_inc(elapsed);
        last_tick = now;
    }

    bsp_wio_buttons_poll();
    cli_poll();
    startup_wifi_poll();
    bsp_wio_wifi_poll();
    bsp_screen_power_poll();
    uint32_t wait_ms = lv_timer_handler();
    if (wait_ms < 1) wait_ms = 1;
    if (wait_ms > 5) wait_ms = 5;
    delay(wait_ms);
}
