/*
 * 串口调试 CLI（仅 esp32）：从 UART0 按行读命令，绕过触屏直接调试网络栈。
 *   help                     命令列表
 *   scan                     扫描 AP 并打印（ssid / rssi / authmode）
 *   wifi <ssid> <pass>     连接 AP（pass 为空则按开放网络连；含空格需整体作为其余行内容）
 *   mr <host> [port]       保存 moonraker.conf 并重连
 *   rx <file> <len>        从串口接收 len 字节原始二进制写入文件（开机图等）
 *   mrstart                按已存配置启动 moonraker 客户端
 *   status                 打印 wifi / moonraker 状态
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "bsp_wifi.h"
#include "app_settings.h"
#include "moonraker_client.h"
#include "klipper_api.h"
#include "printer.h"

#define TAG "cli"
#define LINE_MAX 128

/* ---- 文件系统命令（LittleFS 挂在 /littlefs） ---- */
static char cwd[64] = "/littlefs";

/* 把可能为相对路径的 arg 解析成绝对路径写入 out */
static void fs_resolve(const char *arg, char *out, size_t out_sz)
{
    if (!arg || !arg[0]) { strlcpy(out, cwd, out_sz); return; }
    if (arg[0] == '/')
        strlcpy(out, arg, out_sz);
    else
        snprintf(out, out_sz, "%s/%s", cwd, arg);
}

static void cmd_ls(char *args)
{
    char path[96];
    fs_resolve(args, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) { printf("ls: cannot open %s\n", path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        char full[384];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (!stat(full, &st) && S_ISDIR(st.st_mode))
            printf("  %-24s <dir>\n", e->d_name);
        else
            printf("  %-24s %ld bytes\n", e->d_name, (long)st.st_size);
    }
    closedir(d);
}

static void cmd_cd(char *args)
{
    if (!args || !args[0] || !strcmp(args, ".")) { printf("%s\n", cwd); return; }
    if (!strcmp(args, "..")) {
        char *sl = strrchr(cwd, '/');
        if (sl && sl != cwd) *sl = 0;
        printf("%s\n", cwd);
        return;
    }
    char path[96];
    fs_resolve(args, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) { printf("cd: no such dir %s\n", path); return; }
    closedir(d);
    strlcpy(cwd, path, sizeof(cwd));
    printf("%s\n", cwd);
}

static void cmd_cat(char *args)
{
    char path[96];
    fs_resolve(args, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) { printf("cat: cannot open %s\n", path); return; }
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        printf("%s", buf);
    }
    printf("\n");
    fclose(f);
}

static void cmd_rx(char *args)
{
    /* rx <file> <len>：随后的 len 字节原始二进制从 stdin 直接读入并写文件 */
    char *sp = args ? strchr(args, 0x20) : NULL;
    if (!sp) { printf("usage: rx <file> <len>\n"); return; }
    *sp = 0;
    long len = atol(sp + 1);
    if (len <= 0 || len > 8 * 1024 * 1024) { printf("rx: bad len (%ld)\n", len); return; }
    char path[96];
    fs_resolve(args, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { printf("rx: cannot create %s\n", path); return; }
    printf("rx: receiving %ld bytes to %s, send data now...\n", len, path);
    fflush(stdout);
    static char rbuf[4096];
    long left = len;
    while (left > 0) {
        size_t want = (size_t)(left > (long)sizeof(rbuf) ? sizeof(rbuf) : left);
        size_t got = fread(rbuf, 1, want, stdin);
        if (got == 0) { printf("\nrx: read failed at %ld/%ld\n", len - left, len); break; }
        fwrite(rbuf, 1, got, f);
        left -= (long)got;
    }
    fclose(f);
    printf("rx: done %ld bytes -> %s\n", len - left, path);
}

static void cmd_rm(char *args)
{
    char path[96];
    fs_resolve(args, path, sizeof(path));
    if (!remove(path)) printf("removed %s\n", path);
    else               printf("rm: failed %s\n", path);
}

static const char *wifi_state_str(bsp_wifi_state_t s)
{
    switch (s) {
    case BSP_WIFI_IDLE:       return "IDLE";
    case BSP_WIFI_CONNECTING: return "CONNECTING";
    case BSP_WIFI_CONNECTED:  return "CONNECTED";
    case BSP_WIFI_FAILED:     return "FAILED";
    default:                  return "?";
    }
}

static void cmd_scan(void)
{
    bsp_wifi_scan_start();
    bsp_wifi_ap_t aps[16];
    int n;
    for (int i = 0; i < 50; i++) {          /* 最多等 10s */
        vTaskDelay(pdMS_TO_TICKS(200));
        n = bsp_wifi_scan_poll(aps, 16);
        if (n != BSP_WIFI_SCAN_RUNNING) break;
    }
    if (n <= 0) { printf("scan failed (%d)\n", n); return; }
    for (int i = 0; i < n; i++)
        printf("  %-32s rssi=%d %s\n", aps[i].ssid, aps[i].rssi,
               aps[i].secure ? "secure" : "open");
}

static void cmd_wifi(char *args)
{
    /* args: "<ssid> <pass...>"，ssid 到第一个空格为止，其余整段作密码 */
    char *sp = args ? strchr(args, ' ') : NULL;
    const char *pass = "";
    if (sp) { *sp = 0; pass = sp + 1; while (*pass == ' ') pass++; }
    if (!args || !args[0]) { printf("usage: wifi <ssid> <pass>\n"); return; }
    printf("connecting ssid='%s' pass='%s' (len=%d)\n", args, pass, (int)strlen(pass));
    wifi_conf_t wc = {0};
    strlcpy(wc.ssid, args, sizeof(wc.ssid));
    strlcpy(wc.pass, pass, sizeof(wc.pass));
    wc.valid = true;
    settings_save_wifi(&wc);                /* 同步存 network.conf，下次开机自动连 */
    bsp_wifi_connect(args, pass[0] ? pass : NULL);
}

/* 排查「WiFi 与 RGB DMA 共存导致画面撕裂」用：临时关停/恢复 WiFi 驱动 */
static void cmd_wifioff(void)
{
    esp_wifi_stop();
    printf("wifi stopped (esp_wifi_stop)\n");
}

static void cmd_wifion(void)
{
    esp_wifi_start();
    wifi_conf_t wc = {0};
    if (settings_load_wifi(&wc) && wc.valid)
        bsp_wifi_connect(wc.ssid, wc.pass[0] ? wc.pass : NULL);
    printf("wifi restarted\n");
}

static void cmd_ps(void)
{
    const char *st[] = {"standby", "printing", "paused", "complete",
                        "disconnected", "error"};
    int s = (int)printer_state();
    printf("state=%s ext=%.1f/%.1f bed=%.1f/%.1f pos=%.2f,%.2f,%.2f prog=%.1f%% file='%s'\n",
           (unsigned)s < sizeof(st)/sizeof(st[0]) ? st[s] : "?",
           printer_temp_ext(), printer_target_ext(),
           printer_temp_bed(), printer_target_bed(),
           printer_pos(0), printer_pos(1), printer_pos(2),
           printer_progress_permille() / 10.0f, printer_filename());
}

/* 切换当前打印机槽位（1..6），等价于槽位页的点击——用来从串口复现/验证切槽 */
static void cmd_printer(char *args)
{
    int slot = args ? atoi(args) : 0;
    if (slot < 1 || slot > PRINTER_SLOTS) {
        printf("usage: printer <1-%d>   (active=%d)\n",
               PRINTER_SLOTS, settings_load_active_printer() + 1);
        return;
    }
    settings_save_active_printer(slot - 1);
    moonraker_conf_t mc = {0};
    settings_load_moonraker(&mc);
    printf("active printer -> %d (host='%s' port=%u)\n",
           slot, mc.host, (unsigned)mc.port);
    moonraker_reload();
}

static void cmd_mr(char *args)
{
    moonraker_conf_t mc = {0};
    settings_load_moonraker(&mc);           /* 保留已有 api_key 等 */
    char *sp = args ? strchr(args, ' ') : NULL;
    if (!args || !args[0]) { printf("usage: mr <host> [port]\n"); return; }
    if (sp) { *sp = 0; mc.port = (uint16_t)atoi(sp + 1); }
    strlcpy(mc.host, args, sizeof(mc.host));
    if (!mc.port) mc.port = 7125;
    mc.valid = true;
    settings_save_moonraker(&mc);
    printf("moonraker.conf saved: %s:%u\n", mc.host, mc.port);
    moonraker_reload();
}

static void cmd_gc(char *args)
{
    if (!args || !args[0]) { printf("usage: gc <gcode> (\\ = 换行)\n"); return; }
    char *q = args;
    while (*q) { if (*q == '\\') *q = '\n'; q++; }
    printf("gcode: '%s' -> %s\n", args, klipper_gcode_script(args) ? "sent" : "send failed");
}

static void cli_handle(char *line)
{
    while (*line == ' ') line++;
    char *sp = strchr(line, ' ');
    char *args = NULL;
    if (sp) { *sp = 0; args = sp + 1; }

    if (!strcmp(line, "help")) {
        printf("commands: help | scan | wifi <ssid> <pass> | wifioff | wifion | mr <host> [port] | mrstart | status | ps\n"
               "          printer <1-6> | gc <gcode> | ls [path] | cd <path> | pwd | cat <file> | rm <file> | lcdstat [秒]\n");
    } else if (!strcmp(line, "scan")) {
        cmd_scan();
    } else if (!strcmp(line, "wifi")) {
        cmd_wifi(args);
    } else if (!strcmp(line, "wifioff")) {
        cmd_wifioff();
    } else if (!strcmp(line, "wifion")) {
        cmd_wifion();
    } else if (!strcmp(line, "ps")) {
        cmd_ps();
    } else if (!strcmp(line, "gc")) {
        cmd_gc(args);
    } else if (!strcmp(line, "ls")) {
        cmd_ls(args);
    } else if (!strcmp(line, "cd")) {
        cmd_cd(args);
    } else if (!strcmp(line, "pwd")) {
        printf("%s\n", cwd);
    } else if (!strcmp(line, "cat")) {
        cmd_cat(args);
    } else if (!strcmp(line, "rm")) {
        cmd_rm(args);
    } else if (!strcmp(line, "rx")) {
        cmd_rx(args);   /* 注意：不要在这里调用 cmd_rm —— 会把刚接收的文件删掉 */
    } else if (!strcmp(line, "mr")) {
        cmd_mr(args);
    } else if (!strcmp(line, "printer")) {
        cmd_printer(args);
    } else if (!strcmp(line, "mrstart")) {
        moonraker_start();
        printf("moonraker_start() called\n");
    } else if (!strcmp(line, "lcdstat")) {
#if CONFIG_BOARD_JC8048W550
        extern void bsp_lcd_stats_print(void);
        int secs = args ? atoi(args) : 0;
        if (secs > 0) {
            /* 定时窗口测量：先清零，等 N 秒（用户在此期间滑动），再打印——
               避免待机时 1Hz 时钟渲染污染滑动手感数据 */
            bsp_lcd_stats_print();
            vTaskDelay(pdMS_TO_TICKS(secs * 1000));
        }
        bsp_lcd_stats_print();
#else
        printf("lcdstat: 仅 JC8048W550（rgb44）支持\n");
#endif
    } else if (!strcmp(line, "status")) {
        printf("wifi=%s moonraker=%d rtt=%dms\n", wifi_state_str(bsp_wifi_status()),
               (int)moonraker_state(), printer_rtt_ms());
    } else if (line[0]) {
        printf("unknown: '%s' (try help)\n", line);
    }
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    int  len = 0;
    printf("\ncli ready, try 'help'\n");
    for (;;) {
        int c = getchar();
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (c == '\r' || c == '\n') {
            if (len) {
                line[len] = 0;
                printf("\n");
                cli_handle(line);
                len = 0;
            }
            printf("> ");
            fflush(stdout);
        } else if (c == '\b' || c == 0x7f) {
            if (len) len--;
        } else if (len < LINE_MAX - 1) {
            line[len++] = (char)c;
            putchar(c);                     /* 回显 */
            fflush(stdout);
        }
    }
}

void debug_cli_start(void)
{
    xTaskCreatePinnedToCore(cli_task, "cli", 4096, NULL, 5, NULL, 0);
}
