#pragma once
/*
 * 可复用 ESP32 GPIO 实体按钮后端：把板载方向/功能键翻译成 6 键导航语义事件
 * （上/下/左/右/确定/返回），喂给 ui 层的 ui_buttons 语义层。
 *
 * 板型知识全部留在各板 BSP：板型在 bsp_init 里用 bsp_gpio_buttons_bind()
 * 登记自己的 GPIO 表，本文件不硬编码任何板型引脚。事件经 handler 函数指针
 * 上报（entry 层把它接到 ui_buttons_send()，避免 bsp→ui 组件反向依赖）。
 *
 * 电气灵活性（逐 GPIO 独立配置）：内部上拉 / 下拉 / 浮空，高 / 低电平有效。
 * 同一语义键可绑多个 GPIO（确定键支持 1..3 个）：任一键按下即算按下，
 * 全部抬起才算抬起（后端维护按下计数）。
 *
 * 线程约束：bsp_gpio_buttons_poll() 必须在持 bsp_lvgl_lock 的上下文里周期
 * 调用（各板 lvgl_task，与 bsp_sleep_button_poll 同级），handler 在锁内同步
 * 触发，与 ui_buttons_send() 的 LVGL 锁约定一致。
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 语义键编号：数值与 ui_buttons.h 的 ui_button_id_t 一一对应（entry 层静态断言） */
typedef enum {
    BSP_NAV_KEY_UP = 0,
    BSP_NAV_KEY_DOWN,
    BSP_NAV_KEY_LEFT,
    BSP_NAV_KEY_RIGHT,
    BSP_NAV_KEY_OK,
    BSP_NAV_KEY_BACK,
    BSP_NAV_KEY_COUNT
} bsp_nav_key_t;

typedef enum {
    BSP_GPIO_FLOAT = 0,   /* 浮空（外部电路负责上下拉） */
    BSP_GPIO_PULL_UP,
    BSP_GPIO_PULL_DOWN,
} bsp_gpio_pull_t;

typedef struct {
    int gpio;
    bsp_gpio_pull_t pull;
    bool active_low;
} bsp_gpio_button_cfg_t;

typedef void (*bsp_gpio_button_handler_t)(int key, bool pressed);

/* 把一组 GPIO 绑到同一语义键。同一键可多次 bind 追加，每键上限
   BSP_GPIO_BUTTONS_PER_KEY。登记时采样初态：开机时已按住不算一次按下。 */
esp_err_t bsp_gpio_buttons_bind(bsp_nav_key_t key,
                                const bsp_gpio_button_cfg_t *cfgs, size_t count);

void bsp_gpio_buttons_set_handler(bsp_gpio_button_handler_t fn);

/* 10ms 轮询 + 30ms 消抖；稳定边沿转换为语义事件（息屏时的按下只唤醒、吞掉该次按键）。
   内部自带节拍控制，每次 lvgl_task 循环直接调用即可。 */
void bsp_gpio_buttons_poll(void);

#ifdef __cplusplus
}
#endif
