/* Desktop configuration storage.
 *
 * The released Windows controller stores settings under
 * %APPDATA%\KlipperRemote so upgrades and launch directories don't change
 * the active printer. The simulator deliberately stays portable in its
 * working directory. KLIPPER_CONFIG_DIR overrides both for development/tests.
 */
#include "../bsp_conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <pwd.h>
#endif
#endif

static FILE *open_override(const char *name, const char *mode)
{
    const char *dir = getenv("KLIPPER_CONFIG_DIR");
    if (!dir || !dir[0]) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return fopen(path, mode);
}

static FILE *open_conf(const char *name, const char *mode)
{
    const char *override = getenv("KLIPPER_CONFIG_DIR");
    if (override && override[0]) return open_override(name, mode);

#if defined(_WIN32) && !defined(KLIPPER_DESKTOP_SIMULATOR)
    const wchar_t *appdata = _wgetenv(L"APPDATA");
    if (appdata && appdata[0]) {
        wchar_t dir[MAX_PATH], path[MAX_PATH], wname[80], wmode[8];
        swprintf(dir, MAX_PATH, L"%ls\\KlipperRemote", appdata);
        CreateDirectoryW(dir, NULL);
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname,
                                (int)(sizeof(wname) / sizeof(wname[0]))) &&
            MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode,
                                (int)(sizeof(wmode) / sizeof(wmode[0])))) {
            swprintf(path, MAX_PATH, L"%ls\\%ls", dir, wname);
            FILE *f = _wfopen(path, wmode);
            if (f || mode[0] != 'r') return f;
        }
    }
    /* Read-only fallback migrates settings from older portable builds. */
    if (mode[0] == 'r') return fopen(name, mode);
    return NULL;
#elif !defined(KLIPPER_DESKTOP_SIMULATOR)
    /* Linux/macOS 产品端主配置目录，按优先级：
       1) ~/printer_data/config/KlipperScreen-esp —— Klipper 生态上位机，
          fluidd/mainsail 的 config 页可直接查看/编辑（必须子目录，裸放
          moonraker.conf 会与 Moonraker 自己的配置撞名）
       2) $XDG_CONFIG_HOME/KlipperScreen-esp 或 ~/.config/KlipperScreen-esp */
    const char *home = getenv("HOME");
    const char *base = getenv("XDG_CONFIG_HOME");
    char pd_dir[1024], xdg_dir[1024];
    pd_dir[0] = xdg_dir[0] = 0;
    if (base && base[0])
        snprintf(xdg_dir, sizeof(xdg_dir), "%s/KlipperScreen-esp", base);
    else if (home && home[0])
        snprintf(xdg_dir, sizeof(xdg_dir), "%s/.config/KlipperScreen-esp", home);
    if (home && home[0]) {
        char probe[1024];
        snprintf(probe, sizeof(probe), "%s/printer_data/config", home);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(pd_dir, sizeof(pd_dir), "%s/printer_data/config/KlipperScreen-esp", home);
    }
    const char *primary = pd_dir[0] ? pd_dir : xdg_dir;
    if (!primary[0]) return fopen(name, mode);   /* 无 HOME/XDG：便携 cwd */

    if (mode[0] != 'r') {
        mkdir(primary, 0700);   /* 写只去主目录；父目录由系统保证 */
        char path[1152];
        snprintf(path, sizeof(path), "%s/%s", primary, name);
        return fopen(path, mode);
    }

    /* 读：主目录 → 主目录 .bak 检查点（用户手改损坏时还原已知良好内容）
       → 另一候选目录（printer_data 为主时再看 XDG 旧安装）→ 旧 klipper-remote → cwd */
    char path[1152], bak[96];
    snprintf(bak, sizeof(bak), "%s.bak", name);
    snprintf(path, sizeof(path), "%s/%s", primary, name);
    FILE *f = fopen(path, mode);
    if (f) return f;
    snprintf(path, sizeof(path), "%s/%s", primary, bak);
    f = fopen(path, mode);
    if (f) return f;
    if (pd_dir[0] && xdg_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s", xdg_dir, name);
        f = fopen(path, mode);
        if (f) return f;
        snprintf(path, sizeof(path), "%s/%s", xdg_dir, bak);
        f = fopen(path, mode);
        if (f) return f;
    }
    /* 只读回退迁移：旧 klipper-remote 目录 → 更旧的 cwd 便携存放 */
    if (base && base[0])
        snprintf(path, sizeof(path), "%s/klipper-remote/%s", base, name);
    else
        snprintf(path, sizeof(path), "%s/.config/klipper-remote/%s", home, name);
    f = fopen(path, mode);
    if (f) return f;
    return fopen(name, mode);
#else
    return fopen(name, mode);
#endif
}

int bsp_conf_read(const char *name, char *buf, size_t len)
{
    FILE *f = open_conf(name, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

int bsp_conf_write(const char *name, const char *buf)
{
    FILE *f = open_conf(name, "w");
    if (!f) return -1;
    fputs(buf, f);
    fclose(f);
#if !defined(_WIN32) && !defined(KLIPPER_DESKTOP_SIMULATOR)
    /* 同步刷新 .bak 检查点：应用自己写出的内容视为已知良好。用户经
       fluidd/mainsail 手改主文件改坏时，读路径按 .bak 还原。 */
    char bak[96];
    snprintf(bak, sizeof(bak), "%s.bak", name);
    FILE *b = open_conf(bak, "w");
    if (b) { fputs(buf, b); fclose(b); }
#endif
    return 0;
}

/* 仅 Linux 上位机提供默认打印机：这类机器通常就是 Klipper/Moonraker 本机，
 * 首次开机直接把槽 0 指到本机 127.0.0.1，名称沿用当前登录用户名。 */
bool bsp_conf_default_printer(char *host, size_t host_len, char *name, size_t name_len)
{
#if defined(__linux__) && !defined(KLIPPER_DESKTOP_SIMULATOR)
    if (!host_len || !name_len) return false;
    snprintf(host, host_len, "127.0.0.1");
    const char *user = NULL;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) user = pw->pw_name;
    if (!user) {
        user = getenv("USER");
        if (!user || !user[0]) user = getenv("LOGNAME");
    }
    if (!user || !user[0]) user = "printer";
    snprintf(name, name_len, "%s", user);
    return true;
#else
    (void)host; (void)host_len; (void)name; (void)name_len;
    return false;
#endif
}
