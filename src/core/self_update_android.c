/*
 * Android 自更新（实现 self_update.h 的 supd_* 接口，与 Linux 上位机版同形状）：
 * 关于页"检查更新" → 查询 GitHub latest release（api.github.com）→ 有新版则
 * 下载 KlipperScreen-esp-android.apk 到应用私有目录（跟随 302 重定向，
 * 流式落盘带进度，可暂停/取消）→ .md5 边车校验（mbedtls_md5）→
 * 用户确认后 JNI 调 MainActivity.installApk() 跳系统安装器（FileProvider）。
 * 全程 worker 线程干活，UI 经 supd_poll() 轮询快照，无 LVGL 跨线程访问。
 * HTTPS 传输复用拓竹后端的 bambu_tls 层（mbedTLS + 内嵌 CA bundle）。
 */
#include "self_update.h"

#if defined(__ANDROID__)

#include "version.h"
#include "bambu_tls.h"
#include "cJSON.h"

#include <mbedtls/md5.h>
#include <jni.h>    /* JNIEnv/jobject：SDL.h 的 SDL_main.h 在本 TU 不保证引入 */
#include <SDL.h>   /* SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RELEASE_API  "https://api.github.com/repos/umeiko/KlipperScreen-esp/releases/latest"
#define RELEASE_PAGE "github.com/umeiko/KlipperScreen-esp/releases/latest"
#define ASSET_NAME   "KlipperScreen-esp-android.apk"
#define DL_FILE      "update.apk"
#define DL_FILE_MD5  "update.apk.md5"

static pthread_mutex_t g_lock;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static supd_status_t g_status;
static atomic_int g_busy;        /* worker 在跑 */
static atomic_int g_cancel;      /* 取消请求 */
static atomic_int g_pause;       /* 暂停请求 */
static char g_url[512];          /* 本次下载地址 */
static long g_size;              /* 期望字节数 */

static void once_init(void)
{
    pthread_mutex_init(&g_lock, NULL);
    memset(&g_status, 0, sizeof(g_status));
}

static void publish_state(supd_state_t state)
{
    pthread_mutex_lock(&g_lock);
    g_status.state = state;
    pthread_mutex_unlock(&g_lock);
}

static void publish_error(const char *msg)
{
    pthread_mutex_lock(&g_lock);
    g_status.state = SUPD_ERROR;
    strncpy(g_status.error, msg, sizeof(g_status.error) - 1);
    g_status.error[sizeof(g_status.error) - 1] = 0;
    pthread_mutex_unlock(&g_lock);
}

static void publish_progress(long got, long total)
{
    pthread_mutex_lock(&g_lock);
    g_status.progress = total > 0 ? (int)(got * 1000 / total) : 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---------------- 最小 HTTPS GET（重定向往复 + 流式落盘） ---------------- */

struct url_parts {
    char host[128];
    char path[384];
};

static int split_url(const char *url, struct url_parts *out)
{
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *p = url + 8;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl >= sizeof(out->host)) return -1;
    memcpy(out->host, p, hl);
    out->host[hl] = 0;
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return 0;
}

/* 流式 GET：>0=完成（HTTP 状态码），0=取消，-1=失败。body 写入 out（可为 NULL 丢弃），
 * 或落盘到 file。跟进 301/302/307/308（同 host 或换 host 重连）。 */
static int https_get(const char *url, const char *save_as,
                     char *body_buf, size_t body_cap, long *size_out)
{
    char cur[512];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 4; hop++) {
        struct url_parts u;
        if (split_url(cur, &u) != 0) return -1;
        bambu_tls_t *tls = bambu_tls_connect(u.host, "443", 8000);
        if (!tls) return -1;
        char head[1024];
        int hn = snprintf(head, sizeof(head),
            "GET %s HTTP/1.1\r\nHost: %s\r\n"
            "User-Agent: KlipperScreen-esp-updater\r\n"
            "Accept: */*\r\nConnection: close\r\n\r\n", u.path, u.host);
        if (bambu_tls_write_all(tls, head, hn) != 0) { bambu_tls_close(tls); return -1; }

        /* 状态行 + 头 */
        char line[1024];
        int status = 0;
        long content_length = -1;
        int chunked = 0;
        char location[512] = {0};
        int head_done = 0;
        while (!head_done) {
            int len = 0;
            int idle = 0;
            char prev = 0;
            /* 读一行 */
            while (len < (int)sizeof(line) - 1) {
                int n = bambu_tls_read(tls, line + len, 1);
                if (n < 0) { bambu_tls_close(tls); return -1; }
                if (n == 0) {
                    if (atomic_load(&g_cancel)) { bambu_tls_close(tls); return 0; }
                    if (++idle > 15) { bambu_tls_close(tls); return -1; }
                    continue;
                }
                idle = 0;
                if (line[len] == '\n' && prev == '\r') { len--; break; }
                prev = line[len];
                len++;
            }
            line[len] = 0;
            if (!len) { head_done = 1; break; }
            if (!status) sscanf(line, "HTTP/%*s %d", &status);
            if (strncasecmp(line, "Content-Length:", 15) == 0)
                content_length = strtol(line + 15, NULL, 10);
            if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                strstr(line, "chunked")) chunked = 1;
            if (strncasecmp(line, "Location:", 9) == 0) {
                const char *v = line + 9;
                while (*v == ' ') v++;
                snprintf(location, sizeof(location), "%s", v);
            }
        }
        if (status >= 300 && status < 400 && location[0]) {
            bambu_tls_close(tls);
            if (strncmp(location, "https://", 8) == 0) {
                snprintf(cur, sizeof(cur), "%s", location);
            } else if (location[0] == '/') {
                /* 相对重定向 */
                snprintf(cur, sizeof(cur), "https://%s%s", u.host, location);
            } else {
                return -1;
            }
            continue;
        }
        if (status != 200) { bambu_tls_close(tls); return -1; }

        /* 正文：流式落盘或入缓冲 */
        FILE *fp = save_as ? fopen(save_as, "wb") : NULL;
        if (save_as && !fp) { bambu_tls_close(tls); return -1; }
        long got = 0;
        int fail = 0;
        int idle = 0;
        char buf[8192];
        while (!fail) {
            if (atomic_load(&g_cancel)) { fail = 2; break; }
            while (atomic_load(&g_pause)) {
                if (atomic_load(&g_cancel)) { fail = 2; break; }
                struct timespec ts = { 0, 100000000L };
                nanosleep(&ts, NULL);
            }
            if (fail) break;

            long want;
            size_t chunk_left = 0;
            if (chunked) {
                /* 读 chunk 大小行 */
                char cl[64];
                int len = 0;
                char prev = 0;
                int line_idle = 0;
                while (len < (int)sizeof(cl) - 1) {
                    int n = bambu_tls_read(tls, cl + len, 1);
                    if (n < 0) { fail = 1; break; }
                    if (n == 0) { if (++line_idle > 15) { fail = 1; break; } continue; }
                    line_idle = 0;
                    if (cl[len] == '\n' && prev == '\r') { len--; break; }
                    prev = cl[len];
                    len++;
                }
                if (fail) break;
                cl[len] = 0;
                chunk_left = strtol(cl, NULL, 16);
                if (chunk_left <= 0) break;   /* 尾块 */
                want = (long)sizeof(buf);
                if (want > (long)chunk_left) want = (long)chunk_left;
            } else if (content_length >= 0) {
                if (got >= content_length) break;
                want = (long)sizeof(buf);
                if (want > content_length - got) want = content_length - got;
            } else {
                want = (long)sizeof(buf);
            }
            int n = bambu_tls_read(tls, buf, (int)want);
            if (n < 0) {   /* close 收尾 */
                if (!chunked && content_length < 0) break;
                fail = 1; break;
            }
            if (n == 0) {   /* 读超时：空转预算 30s 防死等 */
                if (++idle > 30) { fail = 1; break; }
                continue;
            }
            idle = 0;
            if (fp) fwrite(buf, 1, (size_t)n, fp);
            else if (body_buf && got + n < (long)body_cap) {
                memcpy(body_buf + got, buf, (size_t)n);
            }
            got += n;
            publish_progress(got, content_length > 0 ? content_length : g_size);
            if (chunked) {
                chunk_left -= n;
                if (chunk_left <= 0) {   /* 吃掉块尾 CRLF（带短超时预算） */
                    char crlf[2];
                    int got2 = 0, idle2 = 0;
                    while (got2 < 2 && idle2 < 4) {
                        int m = bambu_tls_read(tls, crlf + got2, 2 - got2);
                        if (m > 0) got2 += m;
                        else if (m < 0) break;
                        else idle2++;
                    }
                }
            }
        }
        if (fp) fclose(fp);
        if (body_buf && (size_t)got < body_cap) body_buf[got] = 0;
        if (size_out) *size_out = got;
        bambu_tls_close(tls);
        if (fail == 2) return 0;   /* 取消 */
        if (fail) return -1;
        return 1;
    }
    return -1;
}

/* ---------------- 版本比较 ---------------- */
static int semver_cmp(const char *a, const char *b)
{
    int xa[3] = {0}, xb[3] = {0};
    sscanf(a, "%d.%d.%d", &xa[0], &xa[1], &xa[2]);
    sscanf(b, "%d.%d.%d", &xb[0], &xb[1], &xb[2]);
    for (int i = 0; i < 3; i++) {
        if (xa[i] != xb[i]) return xa[i] < xb[i] ? -1 : 1;
    }
    return 0;
}

/* ---------------- worker ---------------- */

static void *update_worker(void *unused)
{
    (void)unused;
    publish_state(SUPD_CHECKING);

    /* 1) latest release 元信息 */
    char *body = malloc(64 * 1024);
    long body_len = 0;
    int rc = body ? https_get(RELEASE_API, NULL, body, 64 * 1024, &body_len) : -1;
    if (rc != 1) {
        free(body);
        if (rc == 0) publish_state(SUPD_IDLE);
        else publish_error("检查失败（网络/GitHub 限流）");
        atomic_store(&g_busy, 0);
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *tag = NULL;
    char dl_url[512] = {0};
    long dl_size = 0;
    if (root) {
        cJSON *t = cJSON_GetObjectItem(root, "tag_name");
        if (cJSON_IsString(t)) tag = t->valuestring;
        cJSON *assets = cJSON_GetObjectItem(root, "assets");
        cJSON *it;
        cJSON_ArrayForEach(it, assets) {
            cJSON *name = cJSON_GetObjectItem(it, "name");
            if (!cJSON_IsString(name) || strcmp(name->valuestring, ASSET_NAME) != 0)
                continue;
            cJSON *u = cJSON_GetObjectItem(it, "browser_download_url");
            cJSON *sz = cJSON_GetObjectItem(it, "size");
            if (cJSON_IsString(u)) {
                strncpy(dl_url, u->valuestring, sizeof(dl_url) - 1);
                dl_size = cJSON_IsNumber(sz) ? (long)sz->valuedouble : 0;
            }
        }
    }
    if (!tag || !dl_url[0]) {
        cJSON_Delete(root);
        publish_error("远端信息不完整");
        atomic_store(&g_busy, 0);
        return NULL;
    }
    const char *latest = tag[0] == 'v' ? tag + 1 : tag;
    pthread_mutex_lock(&g_lock);
    strncpy(g_status.latest, latest, sizeof(g_status.latest) - 1);
    pthread_mutex_unlock(&g_lock);

    if (semver_cmp(latest, KR_VERSION) <= 0) {
        cJSON_Delete(root);
        publish_state(SUPD_UPTODATE);
        atomic_store(&g_busy, 0);
        return NULL;
    }
    cJSON_Delete(root);

    /* 2) 下载 APK（跟随重定向，流式落盘） */
    publish_state(SUPD_DOWNLOADING);
    char path[1100];
    const char *dir = SDL_AndroidGetInternalStoragePath();
    snprintf(path, sizeof(path), "%s%s", dir, DL_FILE);
    snprintf(g_url, sizeof(g_url), "%s", dl_url);
    g_size = dl_size;
    rc = https_get(g_url, path, NULL, 0, NULL);
    if (rc == 0) {   /* 取消 */
        remove(path);
        publish_state(SUPD_IDLE);
        atomic_store(&g_busy, 0);
        return NULL;
    }
    if (rc != 1) {
        remove(path);
        publish_error("下载中断");
        atomic_store(&g_busy, 0);
        return NULL;
    }

    /* 3) md5 校验（.md5 边车） */
    publish_state(SUPD_VERIFYING);
    char md5_url[560];
    snprintf(md5_url, sizeof(md5_url), "%s.md5", g_url);
    char md5_text[128] = {0};
    rc = https_get(md5_url, NULL, md5_text, sizeof(md5_text) - 1, NULL);
    if (rc == 1) {
        /* 计算本地 md5 并比对 */
        unsigned char sum[16];
        mbedtls_md5_context ctx;
        mbedtls_md5_init(&ctx);
        mbedtls_md5_starts(&ctx);
        FILE *fp = fopen(path, "rb");
        if (fp) {
            while (!feof(fp)) {
                unsigned char blk[8192];
                size_t n = fread(blk, 1, sizeof(blk), fp);
                if (n) mbedtls_md5_update(&ctx, blk, n);
            }
            fclose(fp);
        }
        mbedtls_md5_finish(&ctx, sum);
        mbedtls_md5_free(&ctx);
        char hex[33];
        for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", sum[i]);
        if (!strstr(md5_text, hex)) {
            remove(path);
            publish_error("校验失败");
            atomic_store(&g_busy, 0);
            return NULL;
        }
    } else {
        /* 边车缺失不阻断（老 release 没有 APK 的 .md5）：靠安装器签名校验兜底 */
    }

    publish_state(SUPD_READY);
    atomic_store(&g_busy, 0);
    return NULL;
}

/* ---------------- JNI 安装 ---------------- */

static bool jni_install_apk(const char *path)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return false;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID mid = (*env)->GetMethodID(env, cls, "installApk", "(Ljava/lang/String;)V");
    if (!mid) {
        (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, activity);
        return false;
    }
    jstring jpath = (*env)->NewStringUTF(env, path);
    (*env)->CallVoidMethod(env, activity, mid, jpath);
    bool ok = !(*env)->ExceptionCheck(env);
    if (!ok) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jpath);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
    return ok;
}

/* ---------------- supd_* 公共接口 ---------------- */

void supd_check_start(void)
{
    pthread_once(&g_once, once_init);
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_busy, &expected, 1)) return;
    atomic_store(&g_cancel, 0);
    atomic_store(&g_pause, 0);
    pthread_mutex_lock(&g_lock);
    g_status.state = SUPD_CHECKING;
    g_status.progress = 0;
    g_status.error[0] = 0;
    pthread_mutex_unlock(&g_lock);
    pthread_t th;
    if (pthread_create(&th, NULL, update_worker, NULL) != 0) {
        atomic_store(&g_busy, 0);
        publish_error("无法启动任务");
        return;
    }
    pthread_detach(th);
}

void supd_poll(supd_status_t *out)
{
    if (!out) return;
    pthread_once(&g_once, once_init);
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}

void supd_progress_tick(void) { pthread_once(&g_once, once_init); }

void supd_pause(void)
{
    pthread_once(&g_once, once_init);
    pthread_mutex_lock(&g_lock);
    if (g_status.state == SUPD_DOWNLOADING) g_status.state = SUPD_PAUSED;
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_pause, 1);
}

void supd_resume(void)
{
    pthread_once(&g_once, once_init);
    atomic_store(&g_pause, 0);
    pthread_mutex_lock(&g_lock);
    if (g_status.state == SUPD_PAUSED) g_status.state = SUPD_DOWNLOADING;
    pthread_mutex_unlock(&g_lock);
}

void supd_cancel(void)
{
    pthread_once(&g_once, once_init);
    atomic_store(&g_cancel, 1);
    atomic_store(&g_pause, 0);
}

const char *supd_release_page(void) { return RELEASE_PAGE; }
const char *supd_asset_name(void)   { return ASSET_NAME; }

bool supd_apply(void)
{
    pthread_once(&g_once, once_init);
    supd_status_t s;
    supd_poll(&s);
    if (s.state != SUPD_READY) return false;
    char path[1100];
    snprintf(path, sizeof(path), "%s%s",
             SDL_AndroidGetInternalStoragePath(), DL_FILE);
    return jni_install_apk(path);
}

#endif /* __ANDROID__ */
