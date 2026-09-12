/*
 * 串口调试 CLI（仅 esp32）：从 UART0 按行读命令，绕过触屏直接调试网络栈。
 *   help                     命令列表
 *   scan                     扫描 AP 并打印（ssid / rssi / authmode）
 *   wifi <ssid> <pass>     连接 AP（pass 为空则按开放网络连；含空格需整体作为其余行内容）
 *   mr <host> [port]       保存 moonraker.conf 并重连
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
#include "esp_system.h"
#include "esp_heap_caps.h"
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

static void cmd_mem(void)
{
    printf("heap: free=%uB min_ever=%uB largest_blk=%uB\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* ---- 堆泄漏跟踪（heap trace standalone，诊断用） ----
   `ht` 开始跟踪（申请记录缓冲），再敲一次 `ht` 停止并按调用点聚合打印未释放块。
   用法：连上 Moonraker 后 ht → 等 ~10s（泄漏几十 KB）→ ht → 把输出贴给上位机，
   地址用 addr2line 对 ELF 解码。 */
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#define HT_RECORDS 128   /* 记录缓冲 ~7KB；512 条要 ~28KB，会把只剩 45KB 的被测设备自己压死 */

static heap_trace_record_t *ht_buf;

typedef struct { void *caller; size_t bytes; unsigned count; } ht_agg_t;

static void cmd_ht(void)
{
    if (ht_buf) {
        heap_trace_stop();
        heap_trace_summary_t sum;
        heap_trace_summary(&sum);
        printf("trace: allocs=%u frees=%u records=%u%s\n",
               (unsigned)sum.total_allocations, (unsigned)sum.total_frees,
               (unsigned)sum.count, sum.has_overflowed ? " OVERFLOWED(记录不够,结果偏前段)" : "");

        ht_agg_t agg[64];
        int nagg = 0;
        size_t n = heap_trace_get_count();
        for (size_t i = 0; i < n; i++) {
            heap_trace_record_t r;
            if (heap_trace_get(i, &r) != ESP_OK || r.freed || !r.address) continue;
            void *c = r.alloced_by[0];
            int k;
            for (k = 0; k < nagg; k++)
                if (agg[k].caller == c) break;
            if (k == nagg && nagg < 64) { agg[nagg].caller = c; agg[nagg].bytes = 0; agg[nagg].count = 0; nagg++; }
            if (k < nagg) { agg[k].bytes += r.size; agg[k].count++; }
        }
        /* 按字节降序冒泡（量小无所谓） */
        for (int i = 0; i < nagg; i++)
            for (int j = i + 1; j < nagg; j++)
                if (agg[j].bytes > agg[i].bytes) { ht_agg_t t = agg[i]; agg[i] = agg[j]; agg[j] = t; }
        printf("--- 未释放块 Top（caller=malloc 调用点返回地址） ---\n");
        for (int i = 0; i < nagg && i < 15; i++) {
            /* 找回该 caller 的一条完整栈帧便于 addr2line */
            void *frames[4] = {0};
            for (size_t j = 0; j < n; j++) {
                heap_trace_record_t r;
                if (heap_trace_get(j, &r) != ESP_OK || r.freed || !r.address) continue;
                if (r.alloced_by[0] == agg[i].caller) { memcpy(frames, r.alloced_by, sizeof(frames)); break; }
            }
            printf("  %6uB x%-4u caller=%p %p %p %p\n",
                   (unsigned)agg[i].bytes, agg[i].count,
                   frames[0], frames[1], frames[2], frames[3]);
        }
        free(ht_buf);
        ht_buf = NULL;
        return;
    }
    ht_buf = malloc(HT_RECORDS * sizeof(heap_trace_record_t));
    if (!ht_buf) { printf("ht: no mem for record buffer\n"); return; }
    heap_trace_init_standalone(ht_buf, HT_RECORDS);
    heap_trace_start(HEAP_TRACE_LEAKS);
    printf("ht: tracing started (%u records), 等几秒后再敲 ht 停止并聚合\n", HT_RECORDS);
}
#else
static void cmd_ht(void) { printf("ht: 本固件未开 HEAP_TRACING_STANDALONE\n"); }
#endif

/* ---- 每任务堆占用（HEAP_TASK_TRACKING，诊断用） ----
   `taskmem` 打印各任务当前/峰值堆占用，连敲两次对比增长即知哪个任务在漏。 */
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
static void cmd_taskmem(void)
{
    static task_stat_t stats[24];
    static heap_stat_t hstats[48];
    heap_all_tasks_stat_t all = {
        .task_count = 24, .stat_arr = stats,
        .heap_count = 48, .heap_stat_start = hstats,
        .alloc_count = 0, .alloc_stat_start = NULL,
    };
    if (heap_caps_get_all_task_stat(&all) != ESP_OK) { printf("taskmem: failed\n"); return; }
    /* 按 current_usage 降序 */
    size_t n = 0;
    while (n < 24 && stats[n].handle) n++;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (stats[j].overall_current_usage > stats[i].overall_current_usage) {
                task_stat_t t = stats[i]; stats[i] = stats[j]; stats[j] = t;
            }
    printf("task heap usage (current/peak):\n");
    for (size_t i = 0; i < n; i++) {
        if (!stats[i].overall_current_usage && !stats[i].overall_peak_usage) continue;
        printf("  %-14s %7u / %7u B%s\n", stats[i].name,
               (unsigned)stats[i].overall_current_usage,
               (unsigned)stats[i].overall_peak_usage,
               stats[i].is_alive ? "" : " (deleted)");
    }
}
#else
static void cmd_taskmem(void) { printf("taskmem: 本固件未开 HEAP_TASK_TRACKING\n"); }
#endif

static void cli_handle(char *line)
{
    while (*line == ' ') line++;
    char *sp = strchr(line, ' ');
    char *args = NULL;
    if (sp) { *sp = 0; args = sp + 1; }

    if (!strcmp(line, "help")) {
        printf("commands: help | scan | wifi <ssid> <pass> | wifioff | wifion | mr <host> [port] | mrstart | status | ps\n"
               "          printer <1-6> | gc <gcode> | ls [path] | cd <path> | pwd | cat <file> | rm <file> | lcdstat [秒] | mem | ht | taskmem\n");
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
    } else if (!strcmp(line, "mem")) {
        cmd_mem();
    } else if (!strcmp(line, "ht")) {
        cmd_ht();
    } else if (!strcmp(line, "taskmem")) {
        cmd_taskmem();
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
