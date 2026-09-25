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
    /* Linux/macOS 产品端：$XDG_CONFIG_HOME/KlipperScreen-esp 或 ~/.config/KlipperScreen-esp，
       安装位置/工作目录变化不影响已连接的打印机配置。 */
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char dir[1024], path[1152];
    if (base && base[0])
        snprintf(dir, sizeof(dir), "%s/KlipperScreen-esp", base);
    else {
        if (!home || !home[0]) return fopen(name, mode);
        snprintf(dir, sizeof(dir), "%s/.config/KlipperScreen-esp", home);
    }
    mkdir(dir, 0700);   /* 父目录 ~/.config 由系统保证；失败则由 fopen 报错 */
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, mode);
    if (f || mode[0] != 'r') return f;
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
    return 0;
}
