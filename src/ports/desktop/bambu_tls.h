#pragma once

/*
 * Bambu 云通信的 TLS 传输抽象：给 cloud（HTTPS）与 monitor（MQTT）两个
 * POSIX 后端提供统一的"带超时的 TCP 连接 + TLS1.2 会话 + 读写"接口。
 * 唯一实现是 mbedTLS 版（bambu_tls_mbedtls.c）——Linux 桌面与 Android 共用，
 * 静态链接无系统依赖；Windows 桌面走自己的 winhttp/openssl 实现，不用本层。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bambu_tls bambu_tls_t;

/* 建立 TCP 连接（connect 超时 timeout_ms 毫秒）并完成 TLS 握手
 *（SNI + 证书链与主机名校验，TLS>=1.2，CA 用构建期内嵌的 Mozilla bundle）。
 * 成功返回会话句柄，失败返回 NULL。 */
bambu_tls_t *bambu_tls_connect(const char *host, const char *port, int timeout_ms);

/* 读：>0 读到的字节数；0 = 接收超时（约 1 秒，用于轮询式读循环）；
 * -1 = 连接关闭或出错。 */
int bambu_tls_read(bambu_tls_t *tls, void *buf, int len);

/* 写满 len 字节才算成功：0 = 成功，-1 = 失败。 */
int bambu_tls_write_all(bambu_tls_t *tls, const void *buf, int len);

/* 关闭会话并释放资源（NULL 安全）。 */
void bambu_tls_close(bambu_tls_t *tls);

/* 最近一次 read/write/connect 的底层错误码（mbedTLS 负值或 0）。诊断用。 */
int bambu_tls_last_error(const bambu_tls_t *tls);

#ifdef __cplusplus
}
#endif
