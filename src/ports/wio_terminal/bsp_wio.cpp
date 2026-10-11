#include <Arduino.h>
#include <TFT_eSPI.h>
#include <rpcWiFi.h>
#include <sfud.h>
#include <Seeed_Arduino_FreeRTOS.h>

#include <rtl_wifi/wifi_conf.h>
#include <esp/tcpip_adapter.h>

extern "C" {
#include "bsp.h"
#include "bsp_conf.h"
#include "bsp_screen_power.h"
#include "bsp_wifi.h"
#include "ui_buttons.h"
}

#include "bsp_wio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static TFT_eSPI tft;
static lv_display_t *display;
static uint16_t draw_buf[320 * 18];
static const sfud_flash *qspi_flash;
static volatile uint8_t backlight_percent = 100;
static volatile uint8_t backlight_accum;
static volatile uint32_t *backlight_set_reg;
static volatile uint32_t *backlight_clear_reg;
static uint32_t backlight_mask;

extern "C" void TC4_Handler(void)
{
    if (!TC4->COUNT16.INTFLAG.bit.MC0) return;
    TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
    uint8_t percent = backlight_percent;
    if (!percent) {
        *backlight_clear_reg = backlight_mask;
    } else if (percent >= 100) {
        *backlight_set_reg = backlight_mask;
    } else {
        uint16_t next = (uint16_t)backlight_accum + percent;
        if (next >= 100) {
            backlight_accum = (uint8_t)(next - 100);
            *backlight_set_reg = backlight_mask;
        } else {
            backlight_accum = (uint8_t)next;
            *backlight_clear_reg = backlight_mask;
        }
    }
}

static void backlight_pwm_init(void)
{
    uint32_t port = g_APinDescription[LCD_BACKLIGHT].ulPort;
    uint32_t pin = g_APinDescription[LCD_BACKLIGHT].ulPin;
    backlight_set_reg = &PORT->Group[port].OUTSET.reg;
    backlight_clear_reg = &PORT->Group[port].OUTCLR.reg;
    backlight_mask = 1u << pin;

    NVIC_DisableIRQ(TC4_IRQn);
    NVIC_ClearPendingIRQ(TC4_IRQn);
    GCLK->PCHCTRL[TC4_GCLK_ID].reg =
        GCLK_PCHCTRL_GEN_GCLK0_Val | GCLK_PCHCTRL_CHEN;
    TC4->COUNT16.CTRLA.reg &= ~TC_CTRLA_ENABLE;
    while (TC4->COUNT16.SYNCBUSY.reg & TC_SYNCBUSY_ENABLE) {}
    TC4->COUNT16.CTRLA.reg = TC_CTRLA_SWRST;
    while (TC4->COUNT16.SYNCBUSY.reg & TC_SYNCBUSY_SWRST) {}
    while (TC4->COUNT16.CTRLA.bit.SWRST) {}
    TC4->COUNT16.CTRLA.reg =
        TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV64;
    TC4->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
    TC4->COUNT16.CC[0].reg =
        (uint16_t)(SystemCoreClock / 64u / 2000u - 1u);
    while (TC4->COUNT16.SYNCBUSY.reg & TC_SYNCBUSY_CC0) {}
    TC4->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
    NVIC_SetPriority(TC4_IRQn, 6);
    NVIC_EnableIRQ(TC4_IRQn);
    TC4->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
    while (TC4->COUNT16.SYNCBUSY.reg & TC_SYNCBUSY_ENABLE) {}
}

static void lcd_write(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (w <= 0 || h <= 0 || !pixels) return;
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
    tft.pushPixels(const_cast<uint16_t *>(pixels), (uint32_t)w * (uint32_t)h);
    tft.endWrite();
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lcd_write(area->x1, area->y1, w, h,
              reinterpret_cast<const uint16_t *>(px_map));
    lv_display_flush_ready(disp);
}

static void backlight_apply(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    backlight_percent = (uint8_t)percent;
    if (!percent) digitalWrite(LCD_BACKLIGHT, LOW);
    else if (percent >= 100) digitalWrite(LCD_BACKLIGHT, HIGH);
}

static uint64_t clock_ms(void) { return millis(); }

void bsp_init(void)
{
    pinMode(LCD_BACKLIGHT, OUTPUT);
    digitalWrite(LCD_BACKLIGHT, LOW);
    backlight_pwm_init();

    /* A cold-powered ILI9341 can still be inside its own power-on reset when
       the SAMD51 reaches setup().  A warm upload/reset hid this race. */
    delay(120);

    tft.begin();
    tft.setRotation(3);
    tft.setSwapBytes(true);
    tft.fillScreen(TFT_BLACK);
    digitalWrite(LCD_BACKLIGHT, HIGH);
    Serial.println("LCD: ready");

    lv_init();
    display = lv_display_create(320, 240);
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);

    bsp_screen_power_init(backlight_apply, clock_ms);

    /* Keep the QSPI layer deliberately raw: three fixed config sectors at the
       beginning and resource packs from 64KiB onward.  Pulling FatFs into a
       496KiB MCU costs more flash than the filesystem is worth. */
    Serial.print("QSPI: ");
    if (sfud_init() == SFUD_SUCCESS) {
        sfud_flash *flash = sfud_get_device(SFUD_W25Q32_DEVICE_INDEX);
        if (flash && sfud_qspi_fast_read_enable(flash, 2) == SFUD_SUCCESS) {
            qspi_flash = flash;
        }
    }
    if (qspi_flash) {
        Serial.print("ready, ");
        Serial.print((uint32_t)(qspi_flash->chip.capacity / (1024 * 1024)));
        Serial.println(" MiB");
    } else {
        Serial.println("mount failed");
    }
}

const char *bsp_board_name(void) { return "seeed-wio-terminal"; }
void bsp_input_init(void) {}
int bsp_encoder_get_counts_per_detent(void) { return 0; }
int bsp_encoder_default_counts_per_detent(void) { return 0; }
bool bsp_encoder_set_counts_per_detent(int counts) { (void)counts; return false; }
lv_display_t *bsp_get_display(void) { return display; }
void bsp_lvgl_lock(void) {}
void bsp_lvgl_unlock(void) {}
void bsp_restart(void) { NVIC_SystemReset(); }

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    lcd_write(x, y, w, h, px);
}

void bsp_delay_ms(uint32_t ms) { delay(ms); }

void bsp_fade_out(uint32_t ms)
{
    delay(ms);
    backlight_apply(0);
}

bool bsp_disp_can_invert(void) { return true; }
void bsp_disp_set_invert(bool en) { tft.invertDisplay(en); }
bool bsp_disp_can_rotate180(void) { return true; }
void bsp_disp_set_rotate180(bool en) { tft.setRotation(en ? 1 : 3); }
bool bsp_disp_can_mirror_x(void) { return false; }
void bsp_disp_set_mirror_x(bool en) { (void)en; }
bool bsp_time_sync_from_http_date(const char *date)
{
    if (!date || !date[0]) return false;
    char weekday[4], month_text[4], zone[4];
    int day, year, hour, minute, second;
    if (sscanf(date, "%3s, %d %3s %d %d:%d:%d %3s",
               weekday, &day, month_text, &year,
               &hour, &minute, &second, zone) != 8)
        return false;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *month = strstr(months, month_text);
    if (!month || strcmp(zone, "GMT") != 0) return false;

    struct tm tmv = {};
    tmv.tm_mday = day;
    tmv.tm_mon = (int)((month - months) / 3);
    tmv.tm_year = year - 1900;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = second;
    tmv.tm_isdst = -1;
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&tmv);
    setenv("TZ", "CST-8", 1);
    tzset();
    if (epoch <= 0) return false;
    struct timeval tv = { epoch, 0 };
    if (settimeofday(&tv, nullptr) != 0) return false;
    Serial.print("Time: synced from Moonraker: ");
    Serial.println(date);
    return true;
}
void bsp_time_sync_from_host(const char *host, uint16_t port)
{
    (void)host;
    (void)port;
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t crc;
} conf_header_t;

#define CONF_MAGIC       0x3151524Bu  /* "KRQ1" little-endian */
#define CONF_VERSION     1u
#define CONF_SECTOR_SIZE 4096u

static bool conf_offset(const char *name, uint32_t *offset)
{
    if (!name || !offset) return false;
    if (strcmp(name, "network.conf") == 0) *offset = 0x0000;
    else if (strcmp(name, "klipperscreen.conf") == 0) *offset = 0x1000;
    else if (strcmp(name, "moonraker.conf") == 0) *offset = 0x2000;
    else return false;
    return true;
}

static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static int conf_default(const char *name, char *buf, size_t len)
{
    if (!name || strcmp(name, "klipperscreen.conf") != 0) return -1;
    static const char fallback[] = "language=en\n";
    if (sizeof(fallback) > len) return -1;
    memcpy(buf, fallback, sizeof(fallback));
    return (int)sizeof(fallback) - 1;
}

int bsp_conf_read(const char *name, char *buf, size_t len)
{
    if (!buf || len == 0) return -1;
    buf[0] = 0;

    uint32_t offset;
    conf_header_t hdr;
    if (!qspi_flash || !conf_offset(name, &offset) ||
        sfud_read(qspi_flash, offset, sizeof(hdr), (uint8_t *)&hdr) != SFUD_SUCCESS ||
        hdr.magic != CONF_MAGIC || hdr.version != CONF_VERSION ||
        hdr.length >= len || hdr.length > CONF_SECTOR_SIZE - sizeof(hdr))
        return conf_default(name, buf, len);

    if (sfud_read(qspi_flash, offset + sizeof(hdr), hdr.length,
                  (uint8_t *)buf) != SFUD_SUCCESS) {
        buf[0] = 0;
        return conf_default(name, buf, len);
    }
    buf[hdr.length] = 0;
    if (crc32_buf((const uint8_t *)buf, hdr.length) != hdr.crc) {
        buf[0] = 0;
        return conf_default(name, buf, len);
    }
    return hdr.length;
}

int bsp_conf_write(const char *name, const char *buf)
{
    uint32_t offset;
    if (!qspi_flash || !buf || !conf_offset(name, &offset)) return -1;
    size_t len = strlen(buf);
    if (len > CONF_SECTOR_SIZE - sizeof(conf_header_t)) return -1;
    conf_header_t hdr = {
        CONF_MAGIC, CONF_VERSION, (uint16_t)len,
        crc32_buf((const uint8_t *)buf, len)
    };
    if (sfud_erase(qspi_flash, offset, CONF_SECTOR_SIZE) != SFUD_SUCCESS ||
        sfud_write(qspi_flash, offset, sizeof(hdr), (const uint8_t *)&hdr) != SFUD_SUCCESS ||
        sfud_write(qspi_flash, offset + sizeof(hdr), len,
                   (const uint8_t *)buf) != SFUD_SUCCESS)
        return -1;
    return 0;
}

bool bsp_conf_default_printer(char *host, size_t host_len,
                              char *name, size_t name_len)
{
    (void)host; (void)host_len; (void)name; (void)name_len;
    return false;
}

static volatile bsp_wifi_state_t wifi_state = BSP_WIFI_IDLE;
static volatile bool wifi_connect_worker_running;
static bool wifi_initialized;
static bool wifi_disconnect_first;
static uint32_t wifi_connect_started;
static volatile bool wifi_retry_pending;
static uint8_t wifi_retry_remaining;
static uint32_t wifi_retry_at;
static char wifi_connect_ssid[BSP_WIFI_SSID_MAX + 1];
static char wifi_connect_pass[BSP_WIFI_PASS_MAX + 1];
static wifi_auth_mode_t wifi_connect_auth = WIFI_AUTH_MAX;
static char wifi_current_ssid[BSP_WIFI_SSID_MAX + 1];
static char wifi_current_ip[16];
static volatile bool wifi_expected_associated;
static volatile bool wifi_scan_requested;
static volatile bool wifi_network_idle = true;
static bool wifi_scan_started;
static uint32_t wifi_scan_requested_at;

typedef struct {
    char ssid[BSP_WIFI_SSID_MAX + 1];
    wifi_auth_mode_t auth;
} wifi_scan_auth_t;

static wifi_scan_auth_t wifi_scan_auth[16];
static int wifi_scan_auth_count;

static void wifi_event(system_event_t *event)
{
    if (!event) return;
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_CONNECTED: {
        size_t n = event->event_info.connected.ssid_len;
        if (n > BSP_WIFI_SSID_MAX) n = BSP_WIFI_SSID_MAX;
        memcpy(wifi_current_ssid, event->event_info.connected.ssid, n);
        wifi_current_ssid[n] = 0;
        wifi_expected_associated =
            strcmp(wifi_current_ssid, wifi_connect_ssid) == 0;
        break;
    }
    case SYSTEM_EVENT_STA_GOT_IP: {
        /* The RTL8720DN survives a SAMD NVIC reset and may deliver an old
           DHCP event from its previous association.  Never publish that
           lease as the result of the current join. */
        const uint8_t *p = reinterpret_cast<const uint8_t *>(
            &event->event_info.got_ip.ip_info.ip.addr);
        snprintf(wifi_current_ip, sizeof(wifi_current_ip), "%u.%u.%u.%u",
                 p[0], p[1], p[2], p[3]);
        /* WiFi.begin() still owns the RPC client until it returns. Keep the
           public state CONNECTING so Moonraker cannot start another RPC yet. */
        if (!wifi_connect_worker_running && wifi_expected_associated &&
            strcmp(wifi_current_ssid, wifi_connect_ssid) == 0)
            wifi_state = BSP_WIFI_CONNECTED;
        break;
    }
    case SYSTEM_EVENT_STA_DISCONNECTED:
    case SYSTEM_EVENT_STA_LOST_IP:
        wifi_current_ip[0] = 0;
        wifi_current_ssid[0] = 0;
        wifi_expected_associated = false;
        if (!wifi_connect_worker_running) wifi_state = BSP_WIFI_IDLE;
        break;
    default:
        break;
    }
}

static rtw_security_t wifi_rtw_security(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:         return RTW_SECURITY_OPEN;
    case WIFI_AUTH_WEP:          return RTW_SECURITY_WEP_PSK;
    case WIFI_AUTH_WPA_PSK:      return RTW_SECURITY_WPA_AES_PSK;
    case WIFI_AUTH_WPA2_PSK:     return RTW_SECURITY_WPA2_AES_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK: return RTW_SECURITY_WPA_WPA2_MIXED;
    default:                      return RTW_SECURITY_UNKNOWN;
    }
}

static wl_status_t wifi_begin_selected(bool compatibility_fallback)
{
    if (compatibility_fallback && wifi_connect_auth == WIFI_AUTH_WPA_WPA2_PSK)
        return WiFi.begin(wifi_connect_ssid, wifi_connect_pass);

    rtw_security_t security = wifi_rtw_security(wifi_connect_auth);
    if (security == (rtw_security_t)RTW_SECURITY_UNKNOWN)
        return WiFi.begin(wifi_connect_ssid,
                          wifi_connect_pass[0] ? wifi_connect_pass : nullptr);

    char *password = security == RTW_SECURITY_OPEN ? nullptr : wifi_connect_pass;
    int ret = wifi_connect(wifi_connect_ssid, security, password,
                           strlen(wifi_connect_ssid),
                           password ? strlen(password) : 0, -1, nullptr);
    if (ret == RTW_ERROR) return WL_CONNECT_FAILED;
    if (tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA) ==
        ESP_ERR_TCPIP_ADAPTER_DHCPC_START_FAILED)
        return WL_CONNECT_FAILED;
    return WiFi.status();
}

static bool wifi_validate_current_link(void)
{
    if (WiFi.status() != WL_CONNECTED) return false;
    String actual_ssid = WiFi.SSID();
    if (actual_ssid != wifi_connect_ssid) return false;

    IPAddress address = WiFi.localIP();
    if (!(address[0] || address[1] || address[2] || address[3])) return false;
    snprintf(wifi_current_ssid, sizeof(wifi_current_ssid), "%s",
             actual_ssid.c_str());
    snprintf(wifi_current_ip, sizeof(wifi_current_ip), "%u.%u.%u.%u",
             address[0], address[1], address[2], address[3]);
    wifi_expected_associated = true;
    return true;
}

static void wifi_connect_worker(void *)
{
    Serial.print("WiFi: worker connecting to ");
    Serial.println(wifi_connect_ssid);

    /* Disconnecting an idle radio is another synchronous RPC and used to
       double the worst-case wait. Only issue it for a real existing link. */
    bool disconnect_first = wifi_disconnect_first ||
                            wifi_is_connected_to_ap() == RTW_SUCCESS;
    wifi_disconnect_first = false;
    if (disconnect_first) {
        Serial.println("WiFi: clearing stale session");
        (void)wifi_disconnect();
        (void)tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    wifi_current_ip[0] = 0;
    wifi_current_ssid[0] = 0;
    wifi_expected_associated = false;

    /* On the retry, a mixed-mode AP gets the upstream WPA2-AES path. Some
       RTL8720DN firmware accepts the mixed enum but never completes 4-way
       authentication, while the same router accepts a WPA2 station. */
    wl_status_t result = wifi_begin_selected(wifi_retry_remaining == 0);
    /* rpcWiFi can return WL_CONNECT_FAILED just before its asynchronous
       GOT_IP callback arrives.  Publishing FAILED immediately makes the UI
       close the dialog before it can save otherwise-valid credentials. */
    uint32_t settle_started = millis();
    while (result != WL_CONNECTED && wifi_current_ip[0] == 0 &&
           millis() - settle_started < 15000)
        vTaskDelay(pdMS_TO_TICKS(50));

    /* rpcWiFi 1.1.0 occasionally omits SYSTEM_EVENT_STA_CONNECTED even though
       status/SSID/localIP are already valid.  Query the coprocessor after the
       join and validate all three together; this also rejects a delayed IP
       event belonging to an association that survived the SAMD reset. */
    bool connected = wifi_validate_current_link();
    if (connected) {
        wifi_retry_pending = false;
        wifi_retry_remaining = 0;
        snprintf(wifi_current_ssid, sizeof(wifi_current_ssid), "%s",
                 wifi_connect_ssid);
        wifi_state = BSP_WIFI_CONNECTED;
        Serial.print("WiFi: connected, IP ");
        Serial.println(wifi_current_ip);
    } else {
        wifi_current_ip[0] = 0;
        if (wifi_retry_remaining) {
            --wifi_retry_remaining;
            wifi_retry_at = millis() + 5000;
            wifi_retry_pending = true;
            /* Keep CONNECTING visible. A late GOT_IP event can still cancel
               the retry before the next association starts. */
            wifi_state = BSP_WIFI_CONNECTING;
            Serial.print("WiFi: attempt returned status=");
            Serial.print((int)result);
            Serial.println(", retry scheduled");
        } else {
            wifi_state = BSP_WIFI_FAILED;
            Serial.print("WiFi: connect failed, status=");
            Serial.println((int)result);
        }
    }
    wifi_connect_worker_running = false;
    vTaskDelete(nullptr);
}

void bsp_wifi_init(void)
{
    if (wifi_initialized) return;
    wifi_initialized = true;
    WiFi.onEvent(wifi_event);
    if (!WiFi.mode(WIFI_STA)) {
        wifi_state = BSP_WIFI_FAILED;
        Serial.println("WiFi: RTL8720DN init failed");
        return;
    }
    wifi_state = BSP_WIFI_IDLE;
    Serial.println("WiFi: RTL8720DN ready");
}

void bsp_wifi_scan_start(void)
{
    if (wifi_scan_requested) return;
    wifi_scan_started = false;
    wifi_scan_requested_at = millis();
    wifi_scan_requested = true;
    Serial.println("WiFi: scan requested");
}

int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max)
{
    if (!wifi_scan_requested) return BSP_WIFI_SCAN_FAILED;
    if (millis() - wifi_scan_requested_at > 20000) {
        wifi_scan_requested = false;
        wifi_scan_started = false;
        Serial.println("WiFi: scan arbitration timeout");
        return BSP_WIFI_SCAN_FAILED;
    }
    if (wifi_connect_worker_running || !wifi_network_idle)
        return BSP_WIFI_SCAN_RUNNING;
    if (!wifi_scan_started) {
        WiFi.scanDelete();
        int started = WiFi.scanNetworks(true);
        if (started < 0 && started != WIFI_SCAN_RUNNING) {
            wifi_scan_requested = false;
            Serial.println("WiFi: unable to start scan");
            return BSP_WIFI_SCAN_FAILED;
        }
        wifi_scan_started = true;
        Serial.println("WiFi: scan started");
        return BSP_WIFI_SCAN_RUNNING;
    }
    int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return BSP_WIFI_SCAN_RUNNING;
    if (found < 0) {
        wifi_scan_requested = false;
        wifi_scan_started = false;
        return BSP_WIFI_SCAN_FAILED;
    }

    Serial.print("WiFi: scan complete, ");
    Serial.println(found);
    wifi_scan_auth_count = 0;
    int count = 0;
    for (int i = 0; i < found && count < max; ++i) {
        String ssid = WiFi.SSID((uint8_t)i);
        if (ssid.length() == 0) {
            /* RTL8720DN reports one result per hidden BSSID. There is no
               manual-SSID flow in this UI, so blank cards are not actionable. */
            Serial.print("WiFi: skip hidden AP #");
            Serial.println(i);
            continue;
        }

        bool duplicate = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(out[j].ssid, ssid.c_str()) == 0) {
                duplicate = true;
                if (WiFi.RSSI((uint8_t)i) > out[j].rssi)
                    out[j].rssi = WiFi.RSSI((uint8_t)i);
                break;
            }
        }
        if (duplicate) continue;

        wifi_auth_mode_t auth = WiFi.encryptionType((uint8_t)i);
        snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid.c_str());
        out[count].rssi = WiFi.RSSI((uint8_t)i);
        out[count].secure = auth != WIFI_AUTH_OPEN;
        snprintf(wifi_scan_auth[count].ssid, sizeof(wifi_scan_auth[count].ssid),
                 "%s", ssid.c_str());
        wifi_scan_auth[count].auth = auth;
        wifi_scan_auth_count = count + 1;
        Serial.print("WiFi: AP ");
        Serial.print(out[count].ssid);
        Serial.print(" rssi=");
        Serial.print(out[count].rssi);
        Serial.print(" auth=");
        Serial.println((int)auth);
        ++count;
    }
    WiFi.scanDelete();
    wifi_scan_requested = false;
    wifi_scan_started = false;
    return count;
}

bool bsp_wio_wifi_scan_requested(void)
{
    return wifi_scan_requested;
}

void bsp_wio_wifi_network_idle(bool idle)
{
    wifi_network_idle = idle;
}

void bsp_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        wifi_state = BSP_WIFI_FAILED;
        return;
    }
    if (wifi_connect_worker_running) {
        Serial.println("WiFi: connect already in progress");
        return;
    }
    snprintf(wifi_connect_ssid, sizeof(wifi_connect_ssid), "%s", ssid);
    snprintf(wifi_connect_pass, sizeof(wifi_connect_pass), "%s",
             password ? password : "");
    wifi_connect_auth = password && password[0] ? WIFI_AUTH_MAX : WIFI_AUTH_OPEN;
    for (int i = 0; i < wifi_scan_auth_count; ++i) {
        if (strcmp(wifi_scan_auth[i].ssid, wifi_connect_ssid) == 0) {
            wifi_connect_auth = wifi_scan_auth[i].auth;
            break;
        }
    }
    /* Never let an old DHCP lease turn a failed SSID switch into a false
       success. Only this attempt's GOT_IP callback may repopulate the IP. */
    wifi_current_ip[0] = 0;
    wifi_current_ssid[0] = 0;
    wifi_expected_associated = false;
    wifi_disconnect_first = wifi_state == BSP_WIFI_CONNECTED;
    wifi_connect_started = millis();
    wifi_state = BSP_WIFI_CONNECTING;
    wifi_retry_pending = false;
    wifi_retry_remaining = 1;
    wifi_connect_worker_running = true;
    if (xTaskCreate(wifi_connect_worker, "wifi-connect", 1024, nullptr, 3,
                    nullptr) != pdPASS) {
        wifi_connect_worker_running = false;
        wifi_state = BSP_WIFI_FAILED;
        Serial.println("WiFi: unable to allocate connect worker");
    }
}

void bsp_wio_wifi_poll(void)
{
    if (!wifi_retry_pending || wifi_connect_worker_running) return;

    /* The association sometimes completes several seconds after begin()
       returned WL_CONNECT_FAILED. Prefer that valid late event over starting
       a competing RPC request. */
    if (wifi_current_ip[0] && wifi_validate_current_link()) {
        wifi_retry_pending = false;
        wifi_retry_remaining = 0;
        snprintf(wifi_current_ssid, sizeof(wifi_current_ssid), "%s",
                 wifi_connect_ssid);
        wifi_state = BSP_WIFI_CONNECTED;
        Serial.print("WiFi: connected after delayed event, IP ");
        Serial.println(wifi_current_ip);
        return;
    }
    if ((int32_t)(millis() - wifi_retry_at) < 0) return;

    wifi_retry_pending = false;
    wifi_connect_started = millis();
    wifi_state = BSP_WIFI_CONNECTING;
    wifi_connect_worker_running = true;
    Serial.println("WiFi: retrying association");
    if (xTaskCreate(wifi_connect_worker, "wifi-connect", 1024, nullptr, 3,
                    nullptr) != pdPASS) {
        wifi_connect_worker_running = false;
        wifi_state = BSP_WIFI_FAILED;
        Serial.println("WiFi: unable to allocate retry worker");
    }
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    if (wifi_state == BSP_WIFI_CONNECTING &&
        !wifi_connect_worker_running && !wifi_retry_pending &&
        millis() - wifi_connect_started > 30000)
        wifi_state = BSP_WIFI_FAILED;
    return wifi_state;
}

bool bsp_wifi_connected(void)
{
    return wifi_state == BSP_WIFI_CONNECTED;
}

bool bsp_wifi_current(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    if (!bsp_wifi_connected()) {
        if (ssid && ssid_len) ssid[0] = 0;
        if (ip && ip_len) ip[0] = 0;
        return false;
    }
    if (ssid && ssid_len)
        snprintf(ssid, ssid_len, "%s", wifi_current_ssid);
    if (ip && ip_len)
        snprintf(ip, ip_len, "%s", wifi_current_ip);
    return true;
}

typedef struct {
    uint8_t pin;
    ui_button_id_t id;
    bool raw_pressed;
    bool stable_pressed;
    bool swallowed;
    uint32_t changed_ms;
} key_state_t;

static key_state_t keys[] = {
    { WIO_5S_UP,    UI_BTN_UP,    false, false, false, 0 },
    { WIO_5S_DOWN,  UI_BTN_DOWN,  false, false, false, 0 },
    { WIO_5S_LEFT,  UI_BTN_LEFT,  false, false, false, 0 },
    { WIO_5S_RIGHT, UI_BTN_RIGHT, false, false, false, 0 },
    { WIO_5S_PRESS, UI_BTN_OK,    false, false, false, 0 },
    { BUTTON_3,     UI_BTN_BACK,  false, false, false, 0 },
};

static bool buttons_ready;

void bsp_wio_buttons_poll(void)
{
    if (!buttons_ready) {
        for (key_state_t &key : keys) {
            pinMode(key.pin, INPUT_PULLUP);
            key.raw_pressed = key.stable_pressed = digitalRead(key.pin) == LOW;
            key.changed_ms = millis();
        }
        buttons_ready = true;
    }

    uint32_t now = millis();
    for (key_state_t &key : keys) {
        bool pressed = digitalRead(key.pin) == LOW;
        if (pressed != key.raw_pressed) {
            key.raw_pressed = pressed;
            key.changed_ms = now;
        }
        if (pressed == key.stable_pressed || now - key.changed_ms < 25) continue;

        key.stable_pressed = pressed;
        if (pressed) {
            key.swallowed = bsp_screen_activity();
            if (!key.swallowed) ui_buttons_send(key.id, true);
        } else {
            if (!key.swallowed) ui_buttons_send(key.id, false);
            key.swallowed = false;
        }
    }
}
