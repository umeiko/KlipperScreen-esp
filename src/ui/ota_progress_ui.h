#pragma once

/* OTA 升级进度面板：开机自动升级时在屏幕顶层显示进度条。
 * ota_progress_ui_init() 在 UI 构建阶段调用一次（持 LVGL 锁），
 * 内部用 lv_timer 周期轮询进度状态并渲染，无需外部再驱动。 */
void ota_progress_ui_init(void);
void ota_progress_ui_poll(void);
