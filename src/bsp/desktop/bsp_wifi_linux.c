/*
 * WiFi 实现：Linux 桌面（NetworkManager 的 nmcli）
 * 与 Windows 版同一套轮询语义：命令阻塞，放后台线程，UI 轮询状态。
 * 依赖系统已装 network-manager（nmcli 在 PATH 中）。
 */
#include "../bsp_wifi.h"

#if defined(__linux__) && !defined(_WIN32)

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCAN_MAX 24

static struct {
    pthread_mutex_t   lock;
    volatile int      scan_state;   /* 0 idle / 1 running / 2 done / 3 failed */
    bsp_wifi_ap_t     aps[SCAN_MAX];
    int               ap_count;
    volatile bsp_wifi_state_t conn_state;
    volatile int      net_connected; /* 后台轮询缓存：接口当前是否连着任意 AP */
    char              cur_ssid[BSP_WIFI_SSID_MAX + 1]; /* 轮询缓存：当前 AP（lock 保护） */
    char              cur_ip[40];                      /* 轮询缓存：当前 IPv4 */
    int               inited;
    char              target_ssid[BSP_WIFI_SSID_MAX + 1];
    char              password[BSP_WIFI_PASS_MAX + 1];
} W = { .lock = PTHREAD_MUTEX_INITIALIZER, .conn_state = BSP_WIFI_IDLE };

/* nmcli -t 用 '\' 转义值里的分隔符 */
static int split_escaped(char *line, char **fields, int max)
{
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == ':') { *p = 0; fields[n++] = p + 1; }
    }
    return n;
}

static void *scan_thread(void *unused)
{
    (void)unused;
    bsp_wifi_ap_t local[SCAN_MAX];
    int n = 0;

    FILE *fp = popen("nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null", "r");
    if (!fp) { W.scan_state = 3; return NULL; }

    char line[512];
    while (fgets(line, sizeof(line), fp) && n < SCAN_MAX) {
        char *f[3];
        if (split_escaped(line, f, 3) < 3) continue;
        f[2][strcspn(f[2], "\r\n")] = 0;
        if (!f[0][0]) continue;                            /* 隐藏网络 */

        int dup = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(local[i].ssid, f[0]) == 0) { dup = 1; break; }
        if (dup) continue;

        bsp_wifi_ap_t *ap = &local[n++];
        memset(ap, 0, sizeof(*ap));
        strncpy(ap->ssid, f[0], BSP_WIFI_SSID_MAX);
        int pct = atoi(f[1]);
        ap->rssi = pct > 0 ? pct / 2 - 100 : -100;        /* 百分比粗略换算 dBm */
        ap->secure = f[2][0] && strcmp(f[2], "--") != 0;
    }
    pclose(fp);

    pthread_mutex_lock(&W.lock);
    memcpy(W.aps, local, n * sizeof(bsp_wifi_ap_t));
    W.ap_count = n;
    pthread_mutex_unlock(&W.lock);
    W.scan_state = 2;
    return NULL;
}

static void shell_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    out[o++] = '\'';
    for (; *in && o + 5 < out_sz; in++) {
        if (*in == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *in;
    }
    out[o++] = '\'';
    out[o] = 0;
}

static void *connect_thread(void *unused)
{
    (void)unused;
    char ssid_q[160], pass_q[320], cmd[600];
    shell_escape(W.target_ssid, ssid_q, sizeof(ssid_q));
    shell_escape(W.password, pass_q, sizeof(pass_q));
    if (W.password[0])
        snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s password %s 2>&1", ssid_q, pass_q);
    else
        snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s 2>&1", ssid_q);

    /* nmcli connect 自身会阻塞到成功/超时（默认约 25s） */
    FILE *fp = popen(cmd, "r");
    int ok = 0;
    if (fp) {
        char out[256];
        if (fgets(out, sizeof(out), fp) && strstr(out, "successfully activated"))
            ok = 1;
        ok = ok && (pclose(fp) == 0);
    }
    W.conn_state = ok ? BSP_WIFI_CONNECTED : BSP_WIFI_FAILED;
    return NULL;
}

/* 从 `nmcli -t -f <FIELD> dev show <dev>` 输出取第一个值（去掉 "FIELD:" 前缀与尾换行） */
static void nmcli_field(const char *field, const char *dev, char *out, size_t out_sz)
{
    out[0] = 0;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "nmcli -t -f %s dev show '%s' 2>/dev/null", field, dev);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        char *v = strchr(line, ':');
        if (v) {
            v++;
            v[strcspn(v, "\r\n")] = 0;
            char *slash = strchr(v, '/');          /* IP4.ADDRESS 带 /24 前缀长度 */
            if (slash) *slash = 0;
            strncpy(out, v, out_sz - 1);
            out[out_sz - 1] = 0;
        }
    }
    pclose(fp);
}

/* 后台轮询接口真实状态（标题栏图标 + WiFi 面板状态卡用），5 秒一次 */
static void *poll_thread(void *unused)
{
    (void)unused;
    for (;;) {
        FILE *fp = popen("nmcli -t -f TYPE,DEVICE,STATE dev 2>/dev/null", "r");
        int ok = 0;
        char dev[32] = "";
        if (fp) {
            char line[128];
            while (fgets(line, sizeof(line), fp)) {
                /* 格式 wifi:wlan0:connected */
                if (strncmp(line, "wifi:", 5) != 0) continue;
                char *d = line + 5;
                char *sep = strchr(d, ':');
                if (!sep) continue;
                if (strncmp(sep + 1, "connected", 9) != 0) continue;
                *sep = 0;
                strncpy(dev, d, sizeof(dev) - 1);
                ok = 1;
                break;
            }
            pclose(fp);
        }

        char ssid[BSP_WIFI_SSID_MAX + 1] = "", ip[40] = "";
        if (ok && dev[0]) {
            nmcli_field("GENERAL.CONNECTION", dev, ssid, sizeof(ssid));
            nmcli_field("IP4.ADDRESS", dev, ip, sizeof(ip));
        }

        pthread_mutex_lock(&W.lock);
        strncpy(W.cur_ssid, ssid, sizeof(W.cur_ssid) - 1);
        W.cur_ssid[sizeof(W.cur_ssid) - 1] = 0;
        strncpy(W.cur_ip, ip, sizeof(W.cur_ip) - 1);
        W.cur_ip[sizeof(W.cur_ip) - 1] = 0;
        pthread_mutex_unlock(&W.lock);
        W.net_connected = ok;
        sleep(5);
    }
    return NULL;
}

void bsp_wifi_init(void)
{
    if (W.inited) return;
    W.inited = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, poll_thread, NULL) == 0)
        pthread_detach(th);
}

void bsp_wifi_scan_start(void)
{
    if (W.scan_state == 1) return;
    W.scan_state = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, scan_thread, NULL) == 0)
        pthread_detach(th);
    else
        W.scan_state = 3;
}

int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max)
{
    if (W.scan_state == 1) return BSP_WIFI_SCAN_RUNNING;
    if (W.scan_state != 2) return BSP_WIFI_SCAN_FAILED;
    W.scan_state = 0;

    pthread_mutex_lock(&W.lock);
    int n = W.ap_count < max ? W.ap_count : max;
    memcpy(out, W.aps, n * sizeof(bsp_wifi_ap_t));
    pthread_mutex_unlock(&W.lock);
    return n;
}

void bsp_wifi_connect(const char *ssid, const char *password)
{
    if (W.conn_state == BSP_WIFI_CONNECTING) return;
    strncpy(W.target_ssid, ssid, BSP_WIFI_SSID_MAX);
    W.target_ssid[BSP_WIFI_SSID_MAX] = 0;
    strncpy(W.password, password ? password : "", BSP_WIFI_PASS_MAX);
    W.password[BSP_WIFI_PASS_MAX] = 0;
    W.conn_state = BSP_WIFI_CONNECTING;
    pthread_t th;
    if (pthread_create(&th, NULL, connect_thread, NULL) == 0)
        pthread_detach(th);
    else
        W.conn_state = BSP_WIFI_FAILED;
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    return W.conn_state;
}

bool bsp_wifi_connected(void)
{
    return W.net_connected != 0;
}

bool bsp_wifi_current(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    pthread_mutex_lock(&W.lock);
    if (ssid && ssid_len) {
        strncpy(ssid, W.cur_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = 0;
    }
    if (ip && ip_len) {
        strncpy(ip, W.cur_ip, ip_len - 1);
        ip[ip_len - 1] = 0;
    }
    pthread_mutex_unlock(&W.lock);
    return W.net_connected != 0;
}

#endif /* __linux__ && !_WIN32 */
