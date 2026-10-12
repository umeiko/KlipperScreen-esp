#pragma once
/*
 * 配置小文件读写（按文件名）：
 *   esp32   → /littlefs/<name>（LittleFS，bsp_init 时已挂载）
 *   desktop → ./<name>（工作目录，仅调试 UI 用）
 * 内容格式由上层（app_settings）决定，这里只管字节流。
 */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 读整个文件到 buf（ NUL 结尾）。返回读取字节数；文件不存在/失败返回 <0 */
int bsp_conf_read(const char *name, char *buf, size_t len);

/* 整体覆盖写（buf 为 NUL 结尾字符串）。0 成功，<0 失败 */
int bsp_conf_write(const char *name, const char *buf);

/* 可选：首次开机（moonraker.conf 尚不存在）时给打印机槽 0 的平台默认值。
 * 返回 true 表示已填好 host/name；false = 本平台不提供默认（ESP32 等）。
 * Linux 上位机通常就是 Klipper/Moonraker 本机：默认 127.0.0.1 + 当前用户名。 */
bool bsp_conf_default_printer(char *host, size_t host_len, char *name, size_t name_len);

#ifdef __cplusplus
}
#endif
