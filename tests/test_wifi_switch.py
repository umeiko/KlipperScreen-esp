"""Run the real ESP32 Wi-Fi backend against offline SDK/event mocks (requires cc)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
SDK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_TIMEOUT, ESP_ERR_WIFI_NOT_STARTED };
enum { WIFI_EVENT_SCAN_DONE, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_CONNECTED,
       WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, IP_EVENT_STA_GOT_IP };
enum { WIFI_AUTH_OPEN, WIFI_AUTH_WPA_PSK, WIFI_MODE_STA, WIFI_PS_NONE, WIFI_IF_STA,
       ESP_EVENT_ANY_ID=-1, HTTP_EVENT_ON_HEADER=42,
       ESP_ERR_NVS_NO_FREE_PAGES=50, ESP_ERR_NVS_NEW_VERSION_FOUND };
typedef const char *esp_event_base_t;
static const char wifi_event_base[]="wifi",ip_event_base[]="ip";
#define WIFI_EVENT wifi_event_base
#define IP_EVENT ip_event_base
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t name=#name
#define ESP_ERROR_CHECK(expr) assert((expr)==ESP_OK)
static void mock_log(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI(tag, ...) mock_log(tag,__VA_ARGS__)
#define ESP_LOGW(tag, ...) mock_log(tag,__VA_ARGS__)
static const char *esp_err_to_name(int e) {(void)e;return "mock error";}
typedef struct {uint8_t ssid[32],password[64]; struct {int authmode;} threshold;
    struct {bool capable,required;} pmf_cfg;} wifi_sta_config_t;
typedef struct {wifi_sta_config_t sta;} wifi_config_t;
typedef struct {uint8_t ssid[33],bssid[6];int rssi,authmode;} wifi_ap_record_t;
typedef struct {uint8_t ssid[32],ssid_len,bssid[6];} wifi_event_sta_connected_t;
typedef struct {uint8_t ssid[32],ssid_len,bssid[6],reason;} wifi_event_sta_disconnected_t;
typedef struct {int unused;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
static int stop_err,set_err,start_err,connect_err,scan_err,post_err;
static int stops,sets,starts,connects,disconnects;
static int64_t now_us;
static wifi_config_t installed;
static wifi_ap_record_t current_ap;
static bool linked;
static int esp_wifi_stop(void) {++stops;if(!stop_err)linked=false;return stop_err;}
static int esp_wifi_set_config(int i,const wifi_config_t *c) {(void)i;++sets;if(!set_err)installed=*c;return set_err;}
static int esp_wifi_start(void) {++starts;return start_err;}
static int esp_wifi_connect(void) {++connects;return connect_err;}
static int esp_wifi_disconnect(void) {++disconnects;linked=false;return ESP_OK;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *a) {if(!linked)return ESP_FAIL;*a=current_ap;return ESP_OK;}
static int esp_wifi_scan_start(void *c,bool b) {(void)c;(void)b;return scan_err;}
static int esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *a) {*n=0;(void)a;return ESP_OK;}
static int esp_wifi_init(void *c) {(void)c;return ESP_OK;}
static int esp_wifi_set_mode(int m) {(void)m;return ESP_OK;}
static int esp_wifi_set_ps(int m) {(void)m;return ESP_OK;}
typedef struct {esp_event_base_t base;int id;uint8_t data[512];size_t len;} queued_t;
static queued_t queue[16];static int queued;
static int esp_event_post(esp_event_base_t b,int id,const void *d,size_t n,int t) {
    (void)t;if(post_err)return post_err;assert(queued<16 && n<=512);
    queue[queued].base=b;queue[queued].id=id;queue[queued].len=n;
    if(n){memcpy(queue[queued].data,d,n);}++queued;return ESP_OK;
}
static int esp_event_handler_register(esp_event_base_t b,int id,
    void (*f)(void *,esp_event_base_t,int32_t,void *),void *a) {(void)b;(void)id;(void)f;(void)a;return ESP_OK;}
static int esp_event_loop_create_default(void) {return ESP_OK;}
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *);const char *name;bool skip_unhandled_events;} esp_timer_create_args_t;
static int esp_timer_create(const esp_timer_create_args_t *c,esp_timer_handle_t *h) {(void)c;*h=(void *)1;return ESP_OK;}
static int esp_timer_start_periodic(esp_timer_handle_t h,int64_t t) {(void)h;(void)t;return ESP_OK;}
static int64_t esp_timer_get_time(void) {return now_us;}
static int nvs_flash_init(void) {return ESP_OK;}
static int nvs_flash_erase(void) {return ESP_OK;}
typedef struct {uint32_t addr;} mock_ip_t;
typedef struct {mock_ip_t ip;} esp_netif_ip_info_t;
typedef void esp_netif_t;
static int esp_netif_init(void) {return ESP_OK;}
static void *esp_netif_create_default_wifi_sta(void) {return NULL;}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) {(void)k;return NULL;}
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *i) {(void)n;(void)i;return ESP_FAIL;}
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ip) 0,0,0,0
typedef struct {int event_id;char *header_key,*header_value;} esp_http_client_event_t;
typedef struct {const char *url;esp_err_t (*event_handler)(esp_http_client_event_t *);int timeout_ms;} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static void *esp_http_client_init(const esp_http_client_config_t *c) {(void)c;return NULL;}
static int esp_http_client_perform(void *c) {(void)c;return ESP_OK;}
static void esp_http_client_cleanup(void *c) {(void)c;}
#define pdPASS 1
static int xTaskCreate(void (*f)(void *),const char *n,int s,void *a,int p,void *h) {
    (void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 0;
}
static void vTaskDelete(void *t) {(void)t;}
static size_t mock_strlcpy(char *d,const char *s,size_t n) {
    size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;
}
#define strlcpy mock_strlcpy
static char *mock_strptime(const char *s,const char *f,struct tm *t) {(void)s;(void)f;(void)t;return NULL;}
#define strptime mock_strptime
static int mock_settimeofday(const struct timeval *v,void *z) {(void)v;(void)z;return 0;}
#define settimeofday mock_settimeofday
static int mock_setenv(const char *k,const char *v,int replace) {(void)k;(void)v;(void)replace;return 0;}
#define setenv mock_setenv
'''

HARNESS = r'''
#include "sdk.h"
#include "@SOURCE@"
static void event(esp_event_base_t b,int id,void *d) {on_wifi_event(NULL,b,id,d);}
static void pump(void) {
    int n=queued;queued=0;
    for(int i=0;i<n;++i)event(queue[i].base,queue[i].id,queue[i].len?queue[i].data:NULL);
}
static void poll_after(int64_t us) {now_us+=us;event(BSP_WIFI_CONTROL_EVENT,WIFI_CONTROL_POLL,NULL);}
static void reset(void) {
    memset(&W,0,sizeof(W));W.inited=true;W.driver_running=true;W.phase=WIFI_WAIT_IP;
    strcpy(W.target_ssid,"Aaron");memcpy(W.config.sta.ssid,"Aaron",5);
    memset(&current_ap,0,sizeof(current_ap));memcpy(current_ap.ssid,"Aaron",5);
    memset(current_ap.bssid,1,6);memcpy(W.bssid,current_ap.bssid,6);linked=true;
    stops=sets=starts=connects=disconnects=queued=0;
    stop_err=set_err=start_err=connect_err=scan_err=post_err=0;now_us=0;
}
static void request(const char *s) {bsp_wifi_connect(s,"12345678");pump();}
static void start_link(void) {
    if(W.phase==WIFI_STOPPING)event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    assert(W.phase==WIFI_STARTING);event(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    assert(W.phase==WIFI_WAIT_LINK);
}
static void associate(void) {
    wifi_event_sta_connected_t a={0};a.ssid_len=(uint8_t)strlen(W.target_ssid);
    memcpy(a.ssid,W.target_ssid,a.ssid_len);memset(a.bssid,2,6);
    memcpy(current_ap.ssid,W.target_ssid,a.ssid_len+1);memcpy(current_ap.bssid,a.bssid,6);linked=true;
    event(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&a);assert(!bsp_wifi_connected());
}
static void finish(void) {
    start_link();associate();event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_CONNECTED && bsp_wifi_connected());
}
int main(void) {
    /* Reproduce old AP associated without DHCP, including stale events. */
    reset();request("WWW");assert(stops==1 && sets==0 && connects==0);
    event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_CONNECTING);finish();assert(connects==1);
    for(int online=0;online<2;++online) {
        reset();W.phase=online?WIFI_ONLINE:WIFI_WAIT_LINK;W.connected=online;
        request("WWW");assert(!bsp_wifi_connected());finish();
    }
    reset();bsp_wifi_connect("WWW","12345678");event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_CONNECTING && !bsp_wifi_connected());pump();finish();
    /* Coalesce rapid requests; requests own their configuration by value. */
    reset();bsp_wifi_connect("B","12345678");bsp_wifi_connect("C","12345678");pump();
    assert(stops==1 && strcmp(W.target_ssid,"C")==0);request("D");assert(stops==1);
    finish();assert(strcmp((char *)installed.sta.ssid,"D")==0);
    reset();request("Aaron");assert(stops==1);finish();
    reset();W.driver_running=false;request("WWW");assert(stops==0);finish();
    reset();stop_err=ESP_ERR_WIFI_NOT_STARTED;request("WWW");finish();
    /* Every driver/queue failure must end the spinner. */
    reset();stop_err=ESP_FAIL;request("WWW");assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    reset();set_err=ESP_FAIL;request("WWW");event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_FAILED && starts==0);
    reset();start_err=ESP_FAIL;request("WWW");event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    reset();connect_err=ESP_FAIL;request("WWW");event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    event(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    reset();post_err=ESP_FAIL;bsp_wifi_connect("WWW","12345678");
    event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    /* Genuine first-connection failure must not be swallowed as manual disconnect. */
    reset();request("WWW");start_link();event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    assert(bsp_wifi_status()==BSP_WIFI_FAILED && W.retry_at_us==0);
    /* Missing STOP/START and DHCP timeout; late IP must not report success. */
    reset();request("WWW");poll_after(SWITCH_TIMEOUT_US);
    assert(bsp_wifi_status()==BSP_WIFI_FAILED && disconnects==1);
    reset();request("WWW");event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    poll_after(SWITCH_TIMEOUT_US);assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    reset();request("WWW");start_link();associate();poll_after(CONNECT_TIMEOUT_US);
    event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    /* Automatic AP-loss recovery survives repeated failures; explicit stop stays off. */
    reset();request("WWW");finish();event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    poll_after(RECONNECT_DELAY_US);start_link();event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    poll_after(RECONNECT_DELAY_US);finish();
    reset();request("WWW");finish();event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    event(WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    poll_after(RECONNECT_DELAY_US);assert(bsp_wifi_status()==BSP_WIFI_FAILED && W.retry_at_us==0);
    /* AP identity and BSSID checks prevent stale events completing the new connection. */
    reset();request("WWW");start_link();
    wifi_event_sta_connected_t wrong={.ssid="Aaron",.ssid_len=5};
    event(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&wrong);event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
    assert(!bsp_wifi_connected());associate();memset(current_ap.bssid,3,6);
    event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(!bsp_wifi_connected());
    memcpy(current_ap.bssid,W.bssid,6);event(IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
    wifi_event_sta_disconnected_t old={.ssid="Aaron",.ssid_len=5};
    event(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&old);assert(bsp_wifi_connected());
    /* Rejected/cancelled scans must not remain RUNNING. */
    reset();scan_err=ESP_FAIL;bsp_wifi_scan_start();assert(bsp_wifi_scan_poll(NULL,0)==BSP_WIFI_SCAN_FAILED);
    reset();bsp_wifi_scan_start();request("WWW");event(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,NULL);
    assert(bsp_wifi_scan_poll(NULL,0)==BSP_WIFI_SCAN_FAILED);
    /* Maximum-length SSID is not truncated, and invalid input fails immediately. */
    reset();char name[33]="12345678901234567890123456789012";
    bsp_wifi_connect(name,"12345678");name[0]='X';pump();
    assert(W.target_ssid[0]=='1' && strlen(W.target_ssid)==32);finish();assert(installed.sta.ssid[31]=='2');
    reset();bsp_wifi_connect(NULL,NULL);assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    reset();bsp_wifi_connect("123456789012345678901234567890123",NULL);
    assert(bsp_wifi_status()==BSP_WIFI_FAILED);
    puts("ESP32 Wi-Fi switching: all regression scenarios passed");return 0;
}
'''


def main():
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise SystemExit("gcc/cc required (or set CC)")
    with tempfile.TemporaryDirectory(prefix="kr-wifi-switch-") as directory:
        root = Path(directory)
        (root / "sdk.h").write_text(SDK, encoding="utf-8")
        (root / "lvgl.h").write_text("#include <stdint.h>\ntypedef void lv_display_t;\n", encoding="utf-8")
        for header in ("esp_wifi.h", "esp_event.h", "esp_netif.h", "esp_http_client.h",
                       "esp_log.h", "esp_timer.h", "nvs_flash.h",
                       "freertos/FreeRTOS.h", "freertos/task.h"):
            path = root / header
            path.parent.mkdir(exist_ok=True)
            path.write_text('#include "sdk.h"\n', encoding="utf-8")
        source = (REPO / "src/bsp/esp32/bsp_wifi_esp32.c").as_posix()
        (root / "test.c").write_text(HARNESS.replace("@SOURCE@", source), encoding="utf-8")
        exe = root / ("test.exe" if os.name == "nt" else "test")
        subprocess.run([cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root), str(root / "test.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
