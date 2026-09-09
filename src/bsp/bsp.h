#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板级初始化：显示 + 板载主输入 + LVGL 节拍任务 */
void bsp_init(void);

/* 可选附加输入：ESP32 按 Kconfig 建旋钮；desktop 建鼠标滚轮模拟器。 */
void bsp_input_init(void);

lv_display_t *bsp_get_display(void);

/* 旋转编码器输入设备（无编码器/桌面平台返回 NULL）。配合 lv_group 做焦点导航 */
lv_indev_t *bsp_encoder_indev(void);

/* LVGL 线程互斥（所有 LVGL API 调用必须持锁） */
void bsp_lvgl_lock(void);
void bsp_lvgl_unlock(void);

/* 重启设备（esp32=esp_restart，desktop=退出进程；用于语言切换等需重建全部 UI 的场景） */
void bsp_restart(void);

/* 直接把一块 RGB565 像素推上屏 + 毫秒延时（开机动画等 LVGL 场景外渲染用；
   esp32 在 LVGL 任务已启动后调用须先持 bsp_lvgl_lock） */
void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px);
void bsp_delay_ms(uint32_t ms);

/* 用户亮度 0-100。公共状态机保存目标亮度；板型只负责映射到实际背光硬件。 */
void bsp_set_brightness(int pct);

/* 背光从当前值在 ms 内渐变到纯黑（语言切换重启前的淡出；阻塞至渐变完成） */
void bsp_fade_out(uint32_t ms);

/* 自动息屏超时（秒，0=永不）。只更新配置和计时，不隐式唤醒屏幕。 */
void bsp_set_screen_timeout(uint32_t sec);

/* 记录任意输入活动并在需要时唤醒背光；返回 true 表示本次输入刚唤醒屏幕。 */
bool bsp_screen_activity(void);

/* 外部触发的息屏/唤醒（息屏按钮等硬开关用，与自动超时息屏共享同一状态）：
   off = 立即灭背光；wake = 恢复亮度并刷新活动计时；toggle = 原子语义的状态切换。 */
void bsp_screen_off(void);
void bsp_screen_wake(void);
void bsp_screen_toggle(void);
bool bsp_screen_is_off(void);

/* 反色 / 180° 旋转：运行时立即生效（设置项落盘 klipperscreen.conf，
   开机由 app 层读回并调用 setter 应用）。
   can_* 返回本板是否支持，UI 据此隐藏不支持的开关：
   - SPI 屏（CYD ILI9341 / E32R35T ST7796）：两者都支持（panel 命令 + 触摸坐标翻转）；
   - JC8048W550（RGB 并口屏）：面板无命令接口，都不支持；
   - desktop：都不支持（调试前端无此需求）。 */
bool bsp_disp_can_invert(void);
void bsp_disp_set_invert(bool en);
bool bsp_disp_can_rotate180(void);
void bsp_disp_set_rotate180(bool en);

/* 内网时间兜底：从 Moonraker 主机的 HTTP Date 头同步系统时间。
   SNTP 已同步则跳过；异步执行不阻塞调用方（desktop 空操作）。 */
void bsp_time_sync_from_host(const char *host, uint16_t port);

/* 电池电压（mV）与电量百分比（0-100）；未接电池/读取失败返回 -1（UI 显示占位）。
   esp32s3_st7789：IO17=ADC2_CH6，100k:100k 分压（电池电压 = ADC 读数 x2），3.3V=0% / 4.2V=100% 线性；
   其余 esp32 板：未接电池返回 -1；desktop：模拟固定值供调试。 */
int bsp_battery_mv(void);
int bsp_battery_percent(void);

#ifdef __cplusplus
}
#endif
