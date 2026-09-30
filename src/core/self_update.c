/*
 * Linux 上位机自更新实现。依赖目标机自带 curl / md5sum / tar
 *（install.sh 的一键安装链同样依赖 curl，Klipper 上位机必有）。
 * GitHub API 走 https；curl 自动读取 http_proxy/https_proxy 环境变量，
 * 需要代理的用户在 systemd unit 里加 Environment= 即可。
 *
 * 流程：一个 worker 线程串行完成 查询 latest release → 有新版本则直接
 * 下载 desktop-linux-<arch>.tar.gz → 拉 <asset>.md5 边车 → md5sum 校验。
 * 下载进度由 UI 节拍 stat 包体大小得出（worker 阻塞在 waitpid）。
 */
#include "self_update.h"
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST

#include "version.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define API_URL  "https://api.github.com/repos/umeiko/KlipperScreen-esp/releases/latest"
#define WORK_DIR "/tmp/ksesp-update"

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static supd_status_t   st = { SUPD_IDLE, "", "", 0 };
static long            pkg_total;      /* 资产 Content-Length（API 给的 size） */
static pid_t           curl_pid = -1;  /* 进行中的 curl 子进程（暂停/取消用） */
static bool            cancel_req;     /* supd_cancel 置位，worker 出错出口改回 IDLE */

static void set_state(supd_state_t s, const char *err)
{
    pthread_mutex_lock(&st_lock);
    st.state = s;
    if (err) snprintf(st.error, sizeof(st.error), "%s", err);
    else st.error[0] = 0;
    if (s != SUPD_DOWNLOADING) st.progress = 0;
    pthread_mutex_unlock(&st_lock);
}

void supd_poll(supd_status_t *out)
{
    pthread_mutex_lock(&st_lock);
    *out = st;
    pthread_mutex_unlock(&st_lock);
}

/* fork+exec curl，回收退出码；-1=fork/wait 异常，其余为 curl exit code */
static int curl_fetch(const char *url, const char *out_path)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execlp("curl", "curl", "-fsSL", "--connect-timeout", "10",
               "--max-time", "300", "-o", out_path, url, (char *)NULL);
        _exit(127);
    }
    pthread_mutex_lock(&st_lock);
    curl_pid = pid;
    pthread_mutex_unlock(&st_lock);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&st_lock);
    curl_pid = -1;
    pthread_mutex_unlock(&st_lock);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 直连失败时依次走 GitHub 反代镜像（国内网络常见 objects.githubusercontent.com
 * 不通但镜像可通）。md5 边车与包体走同一条链路校验完整性；镜像只做兜底，
 * 直连优先。返回最终 curl 退出码。 */
static const char *mirrors[] = {
    "https://ghproxy.net/",
    "https://gh-proxy.com/",
};

static int fetch_with_fallback(const char *url, const char *out_path)
{
    int rc = curl_fetch(url, out_path);
    for (size_t i = 0; rc != 0 && i < sizeof(mirrors) / sizeof(mirrors[0]); i++) {
        pthread_mutex_lock(&st_lock);
        bool cancelled = cancel_req;
        pthread_mutex_unlock(&st_lock);
        if (cancelled) break;   /* 用户取消：不再换镜像重试 */
        char murl[560];
        snprintf(murl, sizeof(murl), "%s%s", mirrors[i], url);
        rc = curl_fetch(murl, out_path);
    }
    return rc;
}

/* 当前架构对应的 release 资产名：desktop-linux-x86_64/arm64/armhf.tar.gz */
static const char *asset_name(void)
{
    struct utsname u;
    if (uname(&u) == 0) {
        if (strcmp(u.machine, "aarch64") == 0 || strcmp(u.machine, "arm64") == 0)
            return "desktop-linux-arm64.tar.gz";
        if (strncmp(u.machine, "armv", 4) == 0 || strcmp(u.machine, "armhf") == 0)
            return "desktop-linux-armhf.tar.gz";
    }
    return "desktop-linux-x86_64.tar.gz";
}

const char *supd_release_page(void)
{
    return "github.com/umeiko/KlipperScreen-esp/releases/latest";
}

const char *supd_asset_name(void)
{
    return asset_name();
}

/* 三段数字版本比较；"v0.6.3" 的 v 前缀两边都容忍 */
static int version_cmp(const char *a, const char *b)
{
    int aa[3] = {0}, bb[3] = {0};
    if (a[0] == 'v') a++;
    if (b[0] == 'v') b++;
    sscanf(a, "%d.%d.%d", &aa[0], &aa[1], &aa[2]);
    sscanf(b, "%d.%d.%d", &bb[0], &bb[1], &bb[2]);
    for (int i = 0; i < 3; i++)
        if (aa[i] != bb[i]) return aa[i] - bb[i];
    return 0;
}

/* 解析 latest release JSON：找本架构资产，取版本号/下载地址/大小 */
static bool parse_release(const char *json, char *ver, size_t ver_cap,
                          char *url, size_t url_cap, long *size)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (cJSON_IsString(tag) && cJSON_IsArray(assets)) {
        const char *want = asset_name();
        cJSON *it;
        cJSON_ArrayForEach(it, assets) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *dl = cJSON_GetObjectItemCaseSensitive(it, "browser_download_url");
            cJSON *sz = cJSON_GetObjectItemCaseSensitive(it, "size");
            if (!cJSON_IsString(name) || !cJSON_IsString(dl)) continue;
            if (strcmp(name->valuestring, want) != 0) continue;
            snprintf(ver, ver_cap, "%s", tag->valuestring);
            snprintf(url, url_cap, "%s", dl->valuestring);
            *size = cJSON_IsNumber(sz) ? (long)sz->valuedouble : 0;
            ok = true;
            break;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void *update_worker(void *arg)
{
    (void)arg;
    mkdir(WORK_DIR, 0755);

    /* 1. 查询 latest release */
    if (curl_fetch(API_URL, WORK_DIR "/latest.json") != 0) {
        set_state(SUPD_ERROR, "网络错误，无法连接 GitHub");
        return NULL;
    }
    FILE *f = fopen(WORK_DIR "/latest.json", "rb");
    if (!f) { set_state(SUPD_ERROR, "读取响应失败"); return NULL; }
    static char json[64 * 1024];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = 0;

    char ver[24], url[512];
    long size = 0;
    if (!parse_release(json, ver, sizeof(ver), url, sizeof(url), &size)) {
        set_state(SUPD_ERROR, "解析版本信息失败");
        return NULL;
    }
    pthread_mutex_lock(&st_lock);
    snprintf(st.latest, sizeof(st.latest), "%s", ver);
    pkg_total = size;
    pthread_mutex_unlock(&st_lock);

    if (version_cmp(ver, KR_VERSION) <= 0) {
        set_state(SUPD_UPTODATE, NULL);
        return NULL;
    }

    /* 2. 有新版本：直接下载包体 + md5 边车 */
    char pkg[160], md5f[168], md5url[520];
    snprintf(pkg, sizeof(pkg), WORK_DIR "/%s", asset_name());
    snprintf(md5f, sizeof(md5f), "%s.md5", pkg);

    set_state(SUPD_DOWNLOADING, NULL);
    if (fetch_with_fallback(url, pkg) != 0) {
        pthread_mutex_lock(&st_lock);
        bool cancelled = cancel_req;
        pthread_mutex_unlock(&st_lock);
        unlink(pkg);   /* 清掉半截文件，下次从头来 */
        if (cancelled) {   /* 用户取消：静默回 IDLE，不报错 */
            set_state(SUPD_IDLE, NULL);
        } else {
            set_state(SUPD_ERROR, "下载失败");
        }
        return NULL;
    }
    snprintf(md5url, sizeof(md5url), "%s.md5", url);
    if (fetch_with_fallback(md5url, md5f) != 0) {
        set_state(SUPD_ERROR, "缺少校验文件");
        return NULL;
    }

    /* 3. md5sum 计算本地包，与边车前 32 位 hex 对比 */
    set_state(SUPD_VERIFYING, NULL);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "md5sum '%s'", pkg);
    FILE *pp = popen(cmd, "r");
    if (!pp) { set_state(SUPD_ERROR, "无法校验"); return NULL; }
    char got[40] = {0};
    if (!fgets(got, sizeof(got), pp)) got[0] = 0;
    pclose(pp);
    char want[40] = {0};
    FILE *mf = fopen(md5f, "rb");
    if (mf) {
        if (!fgets(want, sizeof(want), mf)) want[0] = 0;
        fclose(mf);
    }
    if (strlen(got) < 32 || strlen(want) < 32 || strncasecmp(got, want, 32) != 0) {
        set_state(SUPD_ERROR, "MD5 校验失败");
        return NULL;
    }
    set_state(SUPD_READY, NULL);
    return NULL;
}

void supd_check_start(void)
{
    pthread_mutex_lock(&st_lock);
    bool busy = (st.state == SUPD_CHECKING || st.state == SUPD_DOWNLOADING ||
                 st.state == SUPD_VERIFYING);
    if (!busy) {
        st.state = SUPD_CHECKING;
        st.error[0] = 0;
        st.progress = 0;
        cancel_req = false;
    }
    pthread_mutex_unlock(&st_lock);
    if (busy) return;

    pthread_t th;
    if (pthread_create(&th, NULL, update_worker, NULL) != 0) {
        set_state(SUPD_ERROR, "无法启动检查线程");
        return;
    }
    pthread_detach(th);
}

/* UI 节拍里调用：用包体已下载字节数推进度（worker 阻塞在 waitpid） */
void supd_progress_tick(void)
{
    pthread_mutex_lock(&st_lock);
    bool dl = (st.state == SUPD_DOWNLOADING);
    long total = pkg_total;
    pthread_mutex_unlock(&st_lock);
    if (!dl || total <= 0) return;

    char pkg[160];
    snprintf(pkg, sizeof(pkg), WORK_DIR "/%s", asset_name());
    struct stat sb;
    if (stat(pkg, &sb) != 0) return;
    int pm = (int)(sb.st_size * 1000 / total);
    if (pm > 1000) pm = 1000;
    pthread_mutex_lock(&st_lock);
    st.progress = pm;
    pthread_mutex_unlock(&st_lock);
}

void supd_pause(void)
{
    pthread_mutex_lock(&st_lock);
    if (st.state == SUPD_DOWNLOADING && curl_pid > 0) {
        kill(curl_pid, SIGSTOP);
        st.state = SUPD_PAUSED;
    }
    pthread_mutex_unlock(&st_lock);
}

void supd_resume(void)
{
    pthread_mutex_lock(&st_lock);
    if (st.state == SUPD_PAUSED && curl_pid > 0) {
        kill(curl_pid, SIGCONT);
        st.state = SUPD_DOWNLOADING;
    }
    pthread_mutex_unlock(&st_lock);
}

void supd_cancel(void)
{
    pthread_mutex_lock(&st_lock);
    if (st.state == SUPD_DOWNLOADING || st.state == SUPD_PAUSED) {
        cancel_req = true;
        if (curl_pid > 0) {
            kill(curl_pid, SIGCONT);   /* 暂停中的进程先放行才能收尸 */
            kill(curl_pid, SIGKILL);
        }
    } else if (st.state == SUPD_READY) {
        st.state = SUPD_IDLE;   /* 放弃已下载的包 */
        st.progress = 0;
    }
    pthread_mutex_unlock(&st_lock);
}

/* READY 后调用：解压并原子替换当前二进制（与 systemd Restart=always 配合：
 * 返回 true 后 UI 调 bsp_restart()，exit 即被拉起新版本） */
bool supd_apply(void)
{
    supd_status_t s;
    supd_poll(&s);
    if (s.state != SUPD_READY) return false;

    char pkg[160];
    snprintf(pkg, sizeof(pkg), WORK_DIR "/%s", asset_name());
    mkdir(WORK_DIR "/x", 0755);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C '%s'", pkg, WORK_DIR "/x");
    if (system(cmd) != 0) return false;

    char newbin[240];
    snprintf(newbin, sizeof(newbin), WORK_DIR "/x/bin/KlipperScreen-esp");
    if (access(newbin, R_OK) != 0) return false;

    /* 当前二进制真实路径；先写 .new 再 rename，避免半截文件 */
    char cur[300];
    ssize_t cl = readlink("/proc/self/exe", cur, sizeof(cur) - 1);
    if (cl <= 0) return false;
    cur[cl] = 0;
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", cur);
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s' && chmod 755 '%s' && mv '%s' '%s'",
             newbin, tmp, tmp, tmp, cur);
    if (system(cmd) != 0) return false;

    /* 启动脚本若随包更新也一并替换（与 bin/ 同级的安装根） */
    char *slash = strrchr(cur, '/');       /* .../bin/KlipperScreen-esp */
    if (slash) {
        *slash = 0;
        char *bin = strrchr(cur, '/');     /* 安装根 */
        if (bin) {
            *bin = 0;
            char newsh[320], cursh[320];
            snprintf(newsh, sizeof(newsh), WORK_DIR "/x/KlipperScreen-esp-start.sh");
            snprintf(cursh, sizeof(cursh), "%s/KlipperScreen-esp-start.sh", cur);
            if (access(newsh, R_OK) == 0 && access(cursh, W_OK) == 0) {
                snprintf(cmd, sizeof(cmd), "cp '%s' '%s' && chmod 755 '%s'",
                         newsh, cursh, cursh);
                system(cmd);
            }
        }
    }
    set_state(SUPD_IDLE, NULL);
    return true;
}

#endif /* BSP_HAS_LINUX_HOST */
