#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板级初始化：显示 + 板载主输入 + LVGL 节拍任务 */
void bsp_init(void);

/* 当前板型的构建名（tools/build-esp32.sh 的板型代号，如 esp32s3-sensecap-indicator；
   desktop 返回 "desktop"）。About 页展示用。 */
const char *bsp_board_name(void);

/* 可选附加输入：ESP32 按 Kconfig 建旋钮；desktop 建鼠标滚轮模拟器。 */
void bsp_input_init(void);

/* 编码器每格正交计数（不是每圈格数）。0 = 编译时板型默认，1..8 = 覆盖。
 * 运行时调用须持 LVGL 锁；切换会丢弃未完成的一格，避免焦点突然跳动。
 * 无编码器后端：getter 返回 0，setter 返回 false。 */
int  bsp_encoder_get_counts_per_detent(void);
int  bsp_encoder_default_counts_per_detent(void);
bool bsp_encoder_set_counts_per_detent(int counts);

lv_display_t *bsp_get_display(void);

/* 旋转编码器输入设备（无编码器/桌面平台返回 NULL）。 */
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
/* 水平镜像（左右翻转）：与 180° 旋转自由组合，触摸坐标同步翻转；
   支持面与 rotate180 相同（SPI 屏），RGB 屏与桌面端隐藏 */
bool bsp_disp_can_mirror_x(void);
void bsp_disp_set_mirror_x(bool en);

/* 面板色序，与反色、RGB565 字节序独立；DEFAULT 保留板型实测默认。
 * 注册成功的后端才展示设置。调用须持 LVGL 锁，false 表示应用失败。
 * SPI LCD 后端及显式桌面预览注册；RGB 并口屏保持原显示路径。 */
typedef enum {
    BSP_COLOR_ORDER_DEFAULT = 0,
    BSP_COLOR_ORDER_RGB = 1,
    BSP_COLOR_ORDER_BGR = 2,
} bsp_color_order_t;
typedef bool (*bsp_color_order_apply_t)(bool bgr);
void bsp_disp_color_order_register(bool default_bgr, bsp_color_order_apply_t apply);
bool bsp_disp_can_color_order(void);
bsp_color_order_t bsp_disp_get_color_order(void);
bool bsp_disp_set_color_order(bsp_color_order_t order);

/* 用标准 HTTP Date（GMT）同步系统时间；供 Moonraker 与云端传输复用。
   返回 Date 是否有效并已应用（desktop 为无操作成功）。 */
bool bsp_time_sync_from_http_date(const char *http_date);

/* 内网时间兜底：异步请求 Moonraker 主机，再把 Date 头交给上面的通用入口。
   调用方不阻塞（desktop 空操作）。 */
void bsp_time_sync_from_host(const char *host, uint16_t port);

/* 电池电压（mV）与电量百分比（0-100）；未接电池/读取失败返回 -1。 */
int bsp_battery_mv(void);
int bsp_battery_percent(void);

/* 快捷键按下时的"选择"回调（slot 0..N）：app 层据此弹确认框。
   板型检测到快捷键按下即调用此回调，不再走编码器旋钮确认。 */
void bsp_hotkey_set_select_handler(void (*handler)(int slot));
/* 执行实际切换（旧接口，保留兼容）。 */
void bsp_hotkey_set_printer_handler(void (*handler)(int slot));

#ifdef __cplusplus
}
#endif
