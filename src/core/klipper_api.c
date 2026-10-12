/*
 * Klipper 控制面薄封装（ESP32/Windows）：组装 JSON-RPC params，经 moonraker_send_rpc 发送。
 * gcode 模板对齐 KlipperScreen ks_includes/KlippyGcodes.py。
 */
#include "klipper_api.h"
#include "moonraker_client.h"

#include <stdio.h>
#include <string.h>

/* 转义 JSON 字符串特殊字符（gcode 里一般不出现引号，防御性处理） */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 3 < out_len; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = *p; }
        else if (*p == '\n')         { out[o++] = '\\'; out[o++] = 'n'; }
        else                           out[o++] = *p;
    }
    out[o] = 0;
}

bool klipper_gcode_script(const char *script)
{
    char esc[512];
    char params[600];
    json_escape(script, esc, sizeof(esc));
    snprintf(params, sizeof(params), "{\"script\":\"%s\"}", esc);
    return moonraker_send_rpc("printer.gcode.script", params);
}

bool klipper_print_start(const char *filename)
{
    char esc[256];
    char params[320];
    json_escape(filename, esc, sizeof(esc));
    snprintf(params, sizeof(params), "{\"filename\":\"%s\"}", esc);
    return moonraker_send_rpc("printer.print.start", params);
}

bool klipper_print_pause(void)  { return moonraker_send_rpc("printer.print.pause", NULL); }
bool klipper_print_resume(void) { return moonraker_send_rpc("printer.print.resume", NULL); }
bool klipper_print_cancel(void) { return moonraker_send_rpc("printer.print.cancel", NULL); }

bool klipper_emergency_stop(void)   { return moonraker_send_rpc("printer.emergency_stop", NULL); }
bool klipper_restart(void)          { return moonraker_send_rpc("printer.restart", NULL); }
bool klipper_firmware_restart(void) { return moonraker_send_rpc("printer.firmware_restart", NULL); }

bool klipper_file_delete(const char *path)
{
    char esc[256];
    char params[320];
    json_escape(path, esc, sizeof(esc));
    snprintf(params, sizeof(params), "{\"path\":\"%s\"}", esc);
    return moonraker_send_rpc("server.files.delete", params);
}
