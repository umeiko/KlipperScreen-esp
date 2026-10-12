/*
 * Linux 上位机硬件联动（仅 BSP_HAS_LINUX_HOST：Linux 桌面，非 ESP32）。
 *
 * - 背光：/sys/class/backlight/<首个条目>/brightness，按 max_brightness 换算。
 *   普通用户默认无权写，install.sh 装 udev 规则（SUBSYSTEM=="backlight"，
 *   group=video + g+w）并把安装用户加进 video 组。
 * - 息屏/唤醒：背光 0/恢复；X11 后端再 best-effort 联动 DPMS（xset）。
 *   Wayland(weston kiosk) 下合成器空闲熄灭已由 --idle-time=0 禁用，屏幕
 *   常亮、靠背光断电达到息屏效果，触摸输入保持活动所以点按即可唤醒。
 * - 电源键：枚举 /dev/input/event*，找 KEY_POWER 能力的设备（红米2/4 是
 *   pm8941_pwrkey），非阻塞读，按下即 bsp_screen_toggle()。安装为服务时
 *   install.sh 会写 logind drop-in（HandlePowerKey=ignore），否则 logind
 *   会先关机。
 */
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST

#include "bsp_linux_host.h"
#include "bsp.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---------- sysfs 背光 ---------- */

static char bl_brightness_path[256];
static int  bl_max = 255;
static int  bl_state;           /* 0=未探测 1=可用 -1=不可用 */
static int  bl_last_pct = -1;

static void backlight_discover(void)
{
    bl_state = -1;
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return;
    struct dirent *ent;
    char base[256] = {0};
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(base, sizeof(base), "/sys/class/backlight/%s", ent->d_name);
        break;
    }
    closedir(d);
    if (!base[0]) return;

    char maxpath[300];
    snprintf(maxpath, sizeof(maxpath), "%s/max_brightness", base);
    FILE *f = fopen(maxpath, "r");
    if (!f) return;
    int m = 0;
    if (fscanf(f, "%d", &m) == 1 && m > 0) bl_max = m;
    fclose(f);

    snprintf(bl_brightness_path, sizeof(bl_brightness_path), "%s/brightness", base);
    if (access(bl_brightness_path, W_OK) != 0) {
        printf("backlight: %s not writable (udev rule missing?)\n", bl_brightness_path);
        return;
    }
    printf("backlight: using %s (max=%d)\n", bl_brightness_path, bl_max);
    bl_state = 1;
}

/* X11 DPMS：仅在 0%<->非0 跳变时调用（xinit 会设 DISPLAY）。
   timeout 兜底——显示栈残缺的机器上 xset 可能挂住，不能阻塞 LVGL 主线程；
   失败仅降级为纯背光控制并记录状态，下次 0/非0 跳变会重试。 */
static int dpms_state;          /* 0=未知/未用 1=上次成功 -1=失败或不可用 */

static void dpms_apply(int pct)
{
    if (bl_last_pct == -1) { bl_last_pct = pct; return; }
    int was_off = bl_last_pct == 0, is_off = pct == 0;
    bl_last_pct = pct;
    if (was_off == is_off) return;
    if (!getenv("DISPLAY")) return;   /* wayland/weston：无 DPMS 概念，背光已够 */
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "timeout 2 xset dpms force %s >/dev/null 2>&1",
             is_off ? "off" : "on");
    if (system(cmd) != 0) {
        if (dpms_state != -1)
            printf("dpms: 'xset dpms force %s' failed, backlight-only\n",
                   is_off ? "off" : "on");
        dpms_state = -1;
    } else {
        dpms_state = 1;
    }
}

/* 上次 DPMS 调用是否成功（仅 X11 路径会尝试；weston 下恒 false） */
int bsp_linux_dpms_ok(void)
{
    return dpms_state == 1;
}

int bsp_linux_backlight_apply(int pct)
{
    if (bl_state == 0) backlight_discover();
    dpms_apply(pct);
    if (bl_state != 1) return 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int v = pct * bl_max / 100;
    int fd = open(bl_brightness_path, O_WRONLY);
    if (fd < 0) return 0;
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "%d", v);
    int ok = write(fd, buf, (size_t)len) == len;
    close(fd);
    return ok;
}

/* ---------- evdev 电源键 ---------- */

static int pwr_fd = -2;         /* -2=未探测 -1=未找到 */

static int test_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * (int)sizeof(unsigned long))] >>
            (bit % (8 * (int)sizeof(unsigned long)))) & 1;
}

static void powerkey_discover(void)
{
    pwr_fd = -1;
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
            test_bit(keybits, KEY_POWER)) {
            char name[128] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            printf("power key: %s (%s)\n", path, name);
            pwr_fd = fd;
            return;
        }
        close(fd);
    }
}

void bsp_linux_powerkey_poll(void)
{
    if (pwr_fd == -2) powerkey_discover();
    if (pwr_fd < 0) return;
    struct input_event ev[8];
    for (;;) {
        ssize_t n = read(pwr_fd, ev, sizeof(ev));
        if (n < (ssize_t)sizeof(ev[0])) break;
        int cnt = (int)(n / (ssize_t)sizeof(ev[0]));
        for (int i = 0; i < cnt; i++) {
            /* 只认按下（value=1）；长按重复（2）与抬起（0）忽略 */
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1)
                bsp_screen_toggle();
        }
    }
}

#endif /* BSP_HAS_LINUX_HOST */
