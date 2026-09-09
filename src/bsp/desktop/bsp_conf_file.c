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
