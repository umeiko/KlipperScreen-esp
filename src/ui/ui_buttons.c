#include "ui_buttons.h"
#include "ui_nav.h"
#include "panel_mgr.h"

static lv_indev_t *buttons_indev;

/* 边沿队列：按下/抬起各为一条记录，indev 每次读取消费一条。
   纯「当前状态」模型会在两次读取之间丢掉快速点按，队列保证边沿不丢；
   队列清空后按 steady state 上报，按住不放仍由 LVGL 做长按重复。 */
typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} key_evt_t;

#define KEY_QUEUE_LEN 16
#define KEY_BACK_SENTINEL 0xFFFFFFFFu   /* 队列里的「返回」伪键：不进 LVGL，读出时执行 */

static key_evt_t key_queue[KEY_QUEUE_LEN];
static unsigned key_q_head, key_q_tail;
static uint32_t act_key = LV_KEY_ENTER;
static lv_indev_state_t act_state = LV_INDEV_STATE_RELEASED;

/* 语义 → LVGL 键值：上/下走 LVGL 组焦点移动（带长按重复），左/右发给控件本身 */
static uint32_t to_lv_key(ui_button_id_t id)
{
    switch (id) {
    case UI_BTN_UP:    return LV_KEY_PREV;
    case UI_BTN_DOWN:  return LV_KEY_NEXT;
    case UI_BTN_LEFT:  return LV_KEY_LEFT;
    case UI_BTN_RIGHT: return LV_KEY_RIGHT;
    default:           return LV_KEY_ENTER;   /* UI_BTN_OK */
    }
}

static void buttons_back(void);   /* read_cb 要用，定义在下 */

static void buttons_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    while (key_q_head != key_q_tail) {
        key_evt_t evt = key_queue[key_q_tail];
        key_q_tail = (key_q_tail + 1) % KEY_QUEUE_LEN;
        if (evt.key == KEY_BACK_SENTINEL) {
            buttons_back();      /* 与导航事件严格保序：先消费完前面的键再返回 */
            continue;
        }
        /* 方向键分派（白名单制；未标记组保持 to_lv_key 原生映射：
           上下 = NEXT/PREV 线性移动，左右原样送达控件，回车确认）：
           1. 屏幕键盘焦点：四键原样送达，键盘 widget 自己走位；
           2. 空间组（面板 create 里 ui_nav_group_set_spatial）：
              非编辑态四键几何就近聚焦；编辑态左右原样调值、上下吞掉；
           3. 列表组（ui_nav_group_set_list）：左 = 返回、右 = 进入/确定。 */
        uint32_t dir = 0;
        switch (evt.key) {
        case LV_KEY_PREV:  dir = LV_KEY_UP;    break;
        case LV_KEY_NEXT:  dir = LV_KEY_DOWN;  break;
        case LV_KEY_LEFT:  dir = LV_KEY_LEFT;  break;
        case LV_KEY_RIGHT: dir = LV_KEY_RIGHT; break;
        }
        if (dir) {
            lv_group_t *g = ui_nav_active_group();
            lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
#if LV_USE_KEYBOARD
            if (f && lv_obj_check_type(f, &lv_keyboard_class)) {
                act_key = dir;
                act_state = evt.state;
                break;
            }
#endif
#if LV_USE_DROPDOWN
            /* 展开的下拉框：四键原样送达，在选项内移动（未展开保持正常导航） */
            if (f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f)) {
                act_key = dir;
                act_state = evt.state;
                break;
            }
#endif
            if (g && ui_nav_group_is_spatial(g)) {
                if (lv_group_get_editing(g)) {
                    if (dir == LV_KEY_LEFT || dir == LV_KEY_RIGHT) {
                        act_key = dir;
                        act_state = evt.state;
                        break;
                    }
                    continue;   /* 编辑中：垂直键不动焦点，保持当前编辑行 */
                }
                if (evt.state == LV_INDEV_STATE_PRESSED) ui_nav_spatial_move(g, dir);
                continue;
            }
            if (g && ui_nav_group_is_list(g)) {
                if (lv_group_get_editing(g) &&
                    (dir == LV_KEY_LEFT || dir == LV_KEY_RIGHT)) {
                    act_key = dir;              /* 编辑态（如 IP 段调值）：左右原样 */
                    act_state = evt.state;
                    break;
                }
                if (dir == LV_KEY_LEFT) {       /* 左 = 返回 */
                    if (evt.state == LV_INDEV_STATE_PRESSED) buttons_back();
                    continue;
                }
                if (dir == LV_KEY_RIGHT)        /* 右 = 进入/确定 */
                    evt.key = LV_KEY_ENTER;
                else
                    evt.key = dir == LV_KEY_UP ? LV_KEY_PREV : LV_KEY_NEXT;
            }
            /* 未标记组：保持原生映射 */
        }
        act_key = evt.key;
        act_state = evt.state;
        break;
    }
    data->key = act_key;
    data->state = act_state;
    data->continue_reading = key_q_head != key_q_tail;
}

static void buttons_back(void)
{
    lv_group_t *g = ui_nav_active_group();
    lv_obj_t *focused = g ? lv_group_get_focused(g) : NULL;

#if LV_USE_DROPDOWN
    /* 打开的下拉列表是页面子层：先收起并还原原选项 */
    if (focused && lv_obj_check_type(focused, &lv_dropdown_class) &&
        lv_dropdown_is_open(focused)) {
        lv_group_send_data(g, LV_KEY_ESC);
        return;
    }
#endif
    if (ui_nav_modal_cancel_top()) return;   /* 弹窗/对话框优先 */
    panel_mgr_back();
}

void ui_buttons_init(void)
{
    if (buttons_indev) return;
    buttons_indev = lv_indev_create();
    lv_indev_set_type(buttons_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(buttons_indev, buttons_read_cb);
    lv_indev_set_display(buttons_indev, lv_display_get_default());
    /* 默认 TIMER 模式周期读取：按住方向键时 LVGL 自动重复焦点移动；
       ui_nav_activate 会把本 indev 绑进各面板/弹层的焦点组 */
}

void ui_buttons_send(ui_button_id_t id, bool pressed)
{
    if (id >= UI_BTN_COUNT) return;
    uint32_t key = id == UI_BTN_BACK ? KEY_BACK_SENTINEL : to_lv_key(id);
    if (id == UI_BTN_BACK && !pressed) return;   /* 返回只看按下沿 */
    unsigned next = (key_q_head + 1) % KEY_QUEUE_LEN;
    if (next == key_q_tail) return;   /* 满：丢新边沿好过错位（持续按住不受影响） */
    key_queue[key_q_head].key = key;
    key_queue[key_q_head].state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    key_q_head = next;
}
