#pragma once
/*
 * Linux 上位机（红米手机/树莓派等）硬件联动：
 * sysfs 背光 + evdev 电源键 + X11 DPMS 息屏。
 * 仅 BSP_HAS_LINUX_HOST=1 的平台编译出实体（见 bsp_linux_host.c）；
 * 其余平台无此头包含路径，不会链接到这些符号。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 背光 0..100 → /sys/class/backlight/<首个条目>/brightness（按 max_brightness 换算）。
 * 0% = 息屏。返回是否真正写到了硬件；false 时调用方走日志兜底。
 * X11 后端内部会联动 DPMS（xset，2s timeout 防挂死 LVGL 线程）。 */
int bsp_linux_backlight_apply(int pct);

/* 上次 DPMS 调用是否成功（仅 X11 路径会尝试；weston 或无 DISPLAY 时恒 false）。
 * 用于调用方判断"息屏是否完全无手段"并向用户提示。 */
int bsp_linux_dpms_ok(void);

/* 主循环周期调用（LVGL 任务上下文，~5ms）：读电源键，按下即息屏/唤醒切换。 */
void bsp_linux_powerkey_poll(void);

#ifdef __cplusplus
}
#endif
