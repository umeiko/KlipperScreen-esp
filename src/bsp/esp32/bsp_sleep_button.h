#pragma once
/*
 * 息屏/唤醒按钮：一个或多个 GPIO 按键，任意一个按下即在息屏/唤醒间切换
 * （屏幕亮 → bsp_screen_off()；屏幕灭 → bsp_screen_wake()）。
 * 按钮列表由各板 BSP 定义（如 BOOT 键、外挂按键），本驱动只负责轮询与消抖。
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    int  gpio;        /* GPIO 号 */
    bool active_low;  /* true=低电平有效（开内部上拉，按键接地）；
                         false=高电平有效（开内部下拉，按键接高电平） */
} bsp_sleep_button_cfg_t;

/* 注册一组息屏按钮并启动轮询任务。可重复调用追加（数组内容会被拷贝）。 */
esp_err_t bsp_sleep_button_init(const bsp_sleep_button_cfg_t *buttons, size_t count);
