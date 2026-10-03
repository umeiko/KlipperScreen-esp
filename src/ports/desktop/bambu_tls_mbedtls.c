/*
 * bambu_tls 的 mbedTLS 实现（Linux 桌面 / Android 共用）。
 * - 非阻塞 connect + poll 实现连接超时；握手/读写回到阻塞 fd（SO_RCVTIMEO 1s，
 *   与 Windows 版行为对齐：读超时返回 0 让调用方轮询）。
 * - 证书校验：构建期由 CMake 把 third_party/cacert.pem（Mozilla bundle）
 *   转成 bambu_ca_bundle.c 内嵌进来，不依赖系统 CA 路径（Android 没有，
 *   Linux 静态发行也不必迁就发行版布局）。
 */
#include "bambu_tls.h"

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>
#include <SDL.h>   /* SDL_Log：Android 落 logcat，Linux 落 stderr/日志文件 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* 由构建系统生成（third_party/cacert.pem → hex 数组，含结尾 NUL） */
extern const unsigned char bambu_ca_bundle[];
extern const unsigned int bambu_ca_bundle_len;

struct bambu_tls {
    int fd;
    int last_err;   /* 最近一次 ssl_read/write/handshake 的原始负错误码 */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
};

static int tcp_connect_timeout(const char *host, const char *port, int timeout_ms)
{
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &result) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *it = result; it && fd < 0; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd); fd = -1; continue;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int rc = poll(&pfd, 1, timeout_ms);
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (rc > 0 && (pfd.revents & POLLOUT) &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) == 0 &&
            so_error == 0) {
            fcntl(fd, F_SETFL, flags);   /* 握手/读写回阻塞模式 */
            break;
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(result);

    if (fd >= 0) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

static int load_ca_bundle(mbedtls_x509_crt *cacert)
{
    static mbedtls_x509_crt bundle;
    static int state;   /* 0=未加载 1=可用 -1=失败 */
    if (state == 0) {
        mbedtls_x509_crt_init(&bundle);
        /* PEM 解析需要把结尾 NUL 计入长度；生成器已保证末尾有 0x00 */
        if (mbedtls_x509_crt_parse(&bundle, bambu_ca_bundle,
                                   bambu_ca_bundle_len) < 0)
            state = -1;
        else
            state = 1;
    }
    if (state != 1) return -1;
    *cacert = bundle;   /* 结构体浅拷贝：conf 只按指针引用链，不能 crt_free 副本 */
    return 0;
}

bambu_tls_t *bambu_tls_connect(const char *host, const char *port, int timeout_ms)
{
    if (!host || !host[0] || timeout_ms <= 0) return NULL;
    bambu_tls_t *tls = calloc(1, sizeof(*tls));
    if (!tls) return NULL;
    tls->fd = -1;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_ctr_drbg_init(&tls->ctr_drbg);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_x509_crt cacert;

    int ok = mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func,
                                   &tls->entropy, NULL, 0) == 0 &&
             load_ca_bundle(&cacert) == 0 &&
             mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ok) {
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&tls->conf, &cacert, NULL);
        mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->ctr_drbg);
        mbedtls_ssl_conf_min_tls_version(&tls->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        ok = mbedtls_ssl_setup(&tls->ssl, &tls->conf) == 0 &&
             mbedtls_ssl_set_hostname(&tls->ssl, host) == 0;
    }
    if (ok) {
        tls->fd = tcp_connect_timeout(host, port, timeout_ms);
        ok = tls->fd >= 0;
        if (!ok) SDL_Log("bambu_tls: tcp connect %s:%s failed", host, port);
    }
    if (ok) {
        mbedtls_ssl_set_bio(&tls->ssl, &tls->fd, mbedtls_net_send,
                            mbedtls_net_recv, NULL);
        int rc;
        while ((rc = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                tls->last_err = rc;
                ok = 0;
                break;
            }
        }
        if (ok && mbedtls_ssl_get_verify_result(&tls->ssl) != 0) ok = 0;
        if (!ok) {
            char errbuf[128];
            mbedtls_strerror(tls->last_err, errbuf, sizeof(errbuf));
            SDL_Log("bambu_tls: handshake %s failed: -0x%04x %s verify=0x%lx",
                    host, -tls->last_err, errbuf,
                    (unsigned long)mbedtls_ssl_get_verify_result(&tls->ssl));
        }
    }
    if (!ok) {
        bambu_tls_close(tls);
        return NULL;
    }
    return tls;
}

int bambu_tls_read(bambu_tls_t *tls, void *buf, int len)
{
    if (!tls || len <= 0) return -1;
    int n = mbedtls_ssl_read(&tls->ssl, buf, (size_t)len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT)
        return 0;   /* SO_RCVTIMEO 到点：调用方轮询 */
    tls->last_err = n;
    return -1;      /* PEER_CLOSE_NOTIFY / 协议错误 / 断开 */
}

int bambu_tls_write_all(bambu_tls_t *tls, const void *buf, int len)
{
    if (!tls || len < 0) return -1;
    const unsigned char *p = buf;
    int done = 0;
    while (done < len) {
        int n = mbedtls_ssl_write(&tls->ssl, p + done, (size_t)(len - done));
        if (n <= 0) {
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            tls->last_err = n;
            return -1;
        }
        done += n;
    }
    return 0;
}

int bambu_tls_last_error(const bambu_tls_t *tls)
{
    return tls ? tls->last_err : 0;
}

void bambu_tls_close(bambu_tls_t *tls)
{
    if (!tls) return;
    mbedtls_ssl_close_notify(&tls->ssl);
    if (tls->fd >= 0) close(tls->fd);
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_ctr_drbg_free(&tls->ctr_drbg);
    mbedtls_entropy_free(&tls->entropy);
    memset(tls, 0, sizeof(*tls));   /* fd/ssl 之外可能有 rng 状态，全清 */
    free(tls);
}
