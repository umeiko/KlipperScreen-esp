/*
 * WiFi 实现：Windows 桌面（netsh wlan）
 * 扫描/连接都是阻塞命令，放进后台线程，UI 轮询状态。
 * netsh 输出是 OEM 代码页（中文系统为 GBK），每行先转成 UTF-8 再解析，
 * 同时兼容英文（"State : connected"）与中文（"状态 : 已连接"）关键词。
 */
#include "../bsp_wifi.h"

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define SCAN_MAX      24
#define CONNECT_TIMEOUT_S 15

static struct {
    CRITICAL_SECTION  lock;
    volatile int      scan_state;   /* 0 idle / 1 running / 2 done / 3 failed */
    bsp_wifi_ap_t     aps[SCAN_MAX];
    int               ap_count;
    volatile bsp_wifi_state_t conn_state;
    volatile int      net_connected; /* 后台轮询缓存：接口当前是否连着任意 AP */
    int               inited;
    char              target_ssid[BSP_WIFI_SSID_MAX + 1];
} W = { .scan_state = 0, .conn_state = BSP_WIFI_IDLE };

static void oem_to_utf8(const char *in, char *out, size_t out_sz)
{
    /* 有的系统（开了 UTF-8 Beta 或 chcp 65001）netsh 直接输出 UTF-8，
       先按 UTF-8 试探，合法则原样保留，否则按 OEM/ACP 代码页转码 */
    wchar_t w[512];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, w, 512) > 0) {
        strncpy(out, in, out_sz - 1);
        out[out_sz - 1] = 0;
        return;
    }
    MultiByteToWideChar(CP_ACP, 0, in, -1, w, 512);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_sz, NULL, NULL);
}

/* 在 GUI 子系统进程中直接调用 _popen/system 会反复创建可见的控制台窗口。
 * 统一通过 CREATE_NO_WINDOW 启动 netsh；需要输出时由匿名管道读取。 */
static int run_netsh_hidden(const wchar_t *args, char **output)
{
    if (output) *output = NULL;

    SECURITY_ATTRIBUTES sa = {
        .nLength = sizeof(sa), .lpSecurityDescriptor = NULL, .bInheritHandle = TRUE
    };
    HANDLE read_pipe = NULL, write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return 0;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH];
    UINT n = GetSystemDirectoryW(exe, MAX_PATH);
    if (!n || n + 12 >= MAX_PATH) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return 0;
    }
    wcscat(exe, L"\\netsh.exe");

    wchar_t cmdline[1024];
    if (swprintf(cmdline, sizeof(cmdline) / sizeof(cmdline[0]),
                 L"\"%ls\" %ls", exe, args) < 0) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return 0;
    }

    STARTUPINFOW si = { .cb = sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    PROCESS_INFORMATION pi = {0};
    BOOL started = CreateProcessW(exe, cmdline, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(write_pipe);
    if (!started) {
        CloseHandle(read_pipe);
        return 0;
    }

    char *data = NULL;
    size_t len = 0, cap = 0;
    int alloc_ok = 1;
    char chunk[1024];
    DWORD got = 0;
    while (ReadFile(read_pipe, chunk, sizeof(chunk), &got, NULL) && got) {
        if (!output || !alloc_ok) continue;
        if (len + got + 1 > cap) {
            size_t next = cap ? cap * 2 : 4096;
            while (next < len + got + 1) next *= 2;
            char *grown = realloc(data, next);
            if (!grown) {
                free(data);
                data = NULL;
                alloc_ok = 0;
                continue;
            }
            data = grown;
            cap = next;
        }
        memcpy(data + len, chunk, got);
        len += got;
    }
    CloseHandle(read_pipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (output && alloc_ok) {
        if (!data) data = malloc(1);
        if (data) data[len] = 0;
        *output = data;
    }
    return exit_code == 0 && (!output || (alloc_ok && data));
}

static char *next_output_line(char **cursor)
{
    if (!cursor || !*cursor || !**cursor) return NULL;
    char *line = *cursor;
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = 0;
        *cursor = nl + 1;
    } else {
        *cursor = line + strlen(line);
    }
    return line;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* ---------- 扫描 ---------- */

static DWORD WINAPI scan_thread(void *unused)
{
    (void)unused;
    bsp_wifi_ap_t local[SCAN_MAX];
    int n = 0, cur = -1;

    char *output = NULL;
    if (!run_netsh_hidden(L"wlan show networks mode=bssid", &output)) {
        free(output);
        W.scan_state = 3;
        return 0;
    }

    char line[512], *cursor = output, *raw;
    while ((raw = next_output_line(&cursor)) != NULL) {
        oem_to_utf8(raw, line, sizeof(line));

        if (strstr(line, "SSID") && !strstr(line, "BSSID")) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            char *ssid = trim(colon + 1);
            if (!ssid[0]) { cur = -1; continue; }          /* 隐藏网络 */
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (strcmp(local[i].ssid, ssid) == 0) { dup = 1; break; }
            if (dup) { cur = -1; continue; }
            if (n >= SCAN_MAX) { cur = -1; continue; }
            cur = n++;
            memset(&local[cur], 0, sizeof(local[cur]));
            strncpy(local[cur].ssid, ssid, BSP_WIFI_SSID_MAX);
        } else if (cur >= 0 && (strstr(line, "Authentication") || strstr(line, "身份验证"))) {
            local[cur].secure = !(strstr(line, "Open") || strstr(line, "开放"));
        } else if (cur >= 0 && local[cur].rssi == 0 &&
                   (strstr(line, "Signal") || strstr(line, "信号"))) {
            int pct = 0;
            if (sscanf(strchr(line, ':') ? strchr(line, ':') + 1 : line, "%d%%", &pct) == 1)
                local[cur].rssi = pct / 2 - 100;           /* 百分比粗略换算 dBm */
        }
    }
    free(output);

    EnterCriticalSection(&W.lock);
    memcpy(W.aps, local, n * sizeof(bsp_wifi_ap_t));
    W.ap_count = n;
    LeaveCriticalSection(&W.lock);
    W.scan_state = 2;
    return 0;
}

/* ---------- 连接 ---------- */

static int check_connected(const char *ssid)
{
    char *output = NULL;
    if (!run_netsh_hidden(L"wlan show interfaces", &output)) {
        free(output);
        return 0;
    }

    int state_ok = 0, ssid_ok = 0;
    char line[512], *cursor = output, *raw;
    while ((raw = next_output_line(&cursor)) != NULL) {
        oem_to_utf8(raw, line, sizeof(line));
        if ((strstr(line, "State") || strstr(line, "状态")) && strchr(line, ':')) {
            char *v = trim(strchr(line, ':') + 1);
            if (strstr(v, "connected") || strstr(v, "已连接")) state_ok = 1;
        } else if (strstr(line, "SSID") && !strstr(line, "BSSID") && strchr(line, ':')) {
            char *v = trim(strchr(line, ':') + 1);
            if (strcmp(v, ssid) == 0) ssid_ok = 1;
        }
    }
    free(output);
    return state_ok && ssid_ok;
}

/* netsh 需要 profile 文件才能连接带密码的网络 */
static void xml_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (; *in && o + 6 < out_sz; in++) {
        const char *rep = NULL;
        if (*in == '&') rep = "&amp;";
        else if (*in == '<') rep = "&lt;";
        else if (*in == '>') rep = "&gt;";
        else if (*in == '"') rep = "&quot;";
        if (rep) { o += snprintf(out + o, out_sz - o, "%s", rep); }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static DWORD WINAPI connect_thread(void *password)
{
    const char *pass = (const char *)password;

    char ssid_x[128], pass_x[256];
    xml_escape(W.target_ssid, ssid_x, sizeof(ssid_x));
    xml_escape(pass ? pass : "", pass_x, sizeof(pass_x));

    wchar_t path[MAX_PATH];
    DWORD path_len = GetTempPathW(MAX_PATH, path);
    if (!path_len || path_len >= MAX_PATH - 22) {
        W.conn_state = BSP_WIFI_FAILED;
        return 0;
    }
    wcscat(path, L"krd_wifi_profile.xml");

    FILE *f = _wfopen(path, L"w");
    if (!f) { W.conn_state = BSP_WIFI_FAILED; return 0; }
    fprintf(f,
        "<?xml version=\"1.0\"?>\n"
        "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\n"
        "  <name>%s</name>\n"
        "  <SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>\n"
        "  <connectionType>ESS</connectionType>\n"
        "  <connectionMode>manual</connectionMode>\n"
        "  <MSM><security>\n"
        "    <authEncryption><authentication>WPA2PSK</authentication>"
        "<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>\n"
        "    <sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
        "<keyMaterial>%s</keyMaterial></sharedKey>\n"
        "  </security></MSM>\n"
        "</WLANProfile>\n", ssid_x, ssid_x, pass_x);
    fclose(f);

    wchar_t args[MAX_PATH + 96];
    swprintf(args, sizeof(args) / sizeof(args[0]),
             L"wlan add profile filename=\"%ls\" user=current", path);
    int profile_ok = run_netsh_hidden(args, NULL);

    wchar_t ssid_w[BSP_WIFI_SSID_MAX + 1];
    int ssid_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       W.target_ssid, -1, ssid_w,
                                       sizeof(ssid_w) / sizeof(ssid_w[0]));
    int connect_ok = 0;
    if (ssid_len > 0) {
        swprintf(args, sizeof(args) / sizeof(args[0]),
                 L"wlan connect name=\"%ls\"", ssid_w);
        connect_ok = run_netsh_hidden(args, NULL);
    }
    DeleteFileW(path);
    if (!profile_ok || !connect_ok) {
        W.conn_state = BSP_WIFI_FAILED;
        return 0;
    }

    for (int i = 0; i < CONNECT_TIMEOUT_S; i++) {
        Sleep(1000);
        if (check_connected(W.target_ssid)) {
            W.conn_state = BSP_WIFI_CONNECTED;
            return 0;
        }
    }
    W.conn_state = BSP_WIFI_FAILED;
    return 0;
}

/* ---------- 接口 ---------- */

/* 后台轮询接口真实状态（标题栏图标用），5 秒一次 */
static DWORD WINAPI poll_thread(void *unused)
{
    (void)unused;
    for (;;) {
        char *output = NULL;
        int queried = run_netsh_hidden(L"wlan show interfaces", &output);
        int ok = 0;
        if (queried) {
            char line[512], *cursor = output, *raw;
            while ((raw = next_output_line(&cursor)) != NULL) {
                oem_to_utf8(raw, line, sizeof(line));
                if ((strstr(line, "State") || strstr(line, "状态")) && strchr(line, ':')) {
                    char *v = trim(strchr(line, ':') + 1);
                    if (strstr(v, "connected") || strstr(v, "已连接")) ok = 1;
                }
            }
        }
        free(output);
        W.net_connected = ok;
        Sleep(5000);
    }
    return 0;
}

void bsp_wifi_init(void)
{
    if (W.inited) return;
    W.inited = 1;
    InitializeCriticalSection(&W.lock);
    CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
}

void bsp_wifi_scan_start(void)
{
    if (W.scan_state == 1) return;
    W.scan_state = 1;
    CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
}

int bsp_wifi_scan_poll(bsp_wifi_ap_t *out, int max)
{
    if (W.scan_state == 1) return BSP_WIFI_SCAN_RUNNING;
    if (W.scan_state != 2) return BSP_WIFI_SCAN_FAILED;
    W.scan_state = 0;

    EnterCriticalSection(&W.lock);
    int n = W.ap_count < max ? W.ap_count : max;
    memcpy(out, W.aps, n * sizeof(bsp_wifi_ap_t));
    LeaveCriticalSection(&W.lock);
    return n;
}

void bsp_wifi_connect(const char *ssid, const char *password)
{
    if (W.conn_state == BSP_WIFI_CONNECTING) return;   /* 简单起见：不抢跑 */
    strncpy(W.target_ssid, ssid, BSP_WIFI_SSID_MAX);
    W.target_ssid[BSP_WIFI_SSID_MAX] = 0;
    W.conn_state = BSP_WIFI_CONNECTING;
    /* password 由调用方保证在连接期间有效（UI 侧用静态缓冲） */
    CreateThread(NULL, 0, connect_thread, (void *)password, 0, NULL);
}

bsp_wifi_state_t bsp_wifi_status(void)
{
    return W.conn_state;
}

bool bsp_wifi_connected(void)
{
    return W.net_connected != 0;
}

#endif /* _WIN32 */
