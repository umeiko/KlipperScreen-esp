#pragma once
/*
 * 语义实体按钮层：6 键导航（上/下/左/右/确定/返回）的共享语义边界。
 *
 * 物理按键后端（desktop SDL 键盘 / 未来 ESP32 GPIO）只负责一件事：
 * 消抖后把「哪个语义键、按下还是抬起」翻译成 ui_buttons_send() 调用。
 * 焦点移动、控件激活、控件调值、返回优先级（弹层 > 面板返回）全部由
 * 本层统一实现，任何后端接入后行为天然一致。
 *
 * 语义约定（与后端无关）：
 *   上/下  = 焦点在组内前移/后移（长按自动重复，由 LVGL keypad 处理）
 *   左/右  = 在值类控件上调节（slider ±、switch 开/关、dropdown 选项移动）；
 *            普通按钮忽略
 *   确定   = 激活聚焦控件（按钮 click / switch toggle / dropdown 展开或选定）
 *   返回   = 收起打开的下拉 → 取消顶层弹层/对话框 → 面板返回
 *
 * ── 未来 ESP32 GPIO 后端约定（本阶段不实现，仅确立扩展边界）──
 * - 每个语义按钮可绑定 1..N 个 GPIO：新板型上「确定」可并挂 1~3 个物理键，
 *   任一键按下即视为该语义键按下，全部抬起才算抬起（后端维护按下计数）；
 * - 每个 GPIO 独立配置：内部上拉 / 下拉 / 浮空，高电平 / 低电平有效。
 *   后端在采样侧把电平归一化成 pressed/released 逻辑态再上报，
 *   本层不感知任何电气细节，也不引入板型配置；
 * - 线程约束：ui_buttons_send() 须在 LVGL 锁内调用
 *   （desktop 在 SDL 事件回调里天然满足；ESP 后端应在持 bsp_lvgl_lock 的
 *     轮询/消抖路径里调用，与 bsp_rotary_encoder 的读回调同级）。
 */
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_BTN_UP = 0,
    UI_BTN_DOWN,
    UI_BTN_LEFT,
    UI_BTN_RIGHT,
    UI_BTN_OK,
    UI_BTN_BACK,
    UI_BTN_COUNT
} ui_button_id_t;

/* 建共享 keypad indev（ui_app_create 在 ui_nav_init 之后调用，幂等） */
void ui_buttons_init(void);

/* 后端唯一入口：语义键按下/抬起（pressed=false 仅对方向/确定有意义） */
void ui_buttons_send(ui_button_id_t id, bool pressed);

#ifdef __cplusplus
}
#endif
