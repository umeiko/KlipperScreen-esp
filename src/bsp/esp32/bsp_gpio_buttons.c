#include "bsp_gpio_buttons.h"

#include "bsp.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define POLL_PERIOD_MS           10
#define DEBOUNCE_MS              30
#define BSP_GPIO_BUTTONS_PER_KEY 3   /* 每语义键可挂的 GPIO 上限（确定键需求 1..3） */

static const char *TAG = "gpio_buttons";

typedef struct {
    int     gpio;
    bool    active_low;
    bool    stable_pressed;   /* 消抖后的稳定状态 */
    bool    raw_pressed;      /* 上一次采样到的原始状态 */
    int64_t raw_since_us;     /* 原始状态最近一次变化的时间 */
} gpio_btn_t;

typedef struct {
    gpio_btn_t btns[BSP_GPIO_BUTTONS_PER_KEY];
    size_t     count;
    int        pressed_cnt;   /* 当前处于按下态的 GPIO 数 */
    bool       reported;      /* 本次按下的按下沿已上报（息屏唤醒吞键时为假，抬起也不报） */
} nav_key_t;

static nav_key_t keys[BSP_NAV_KEY_COUNT];
static bsp_gpio_button_handler_t handler;
static int64_t next_poll_us;

static bool read_pressed(const gpio_btn_t *b)
{
    return gpio_get_level(b->gpio) == (b->active_low ? 0 : 1);
}

esp_err_t bsp_gpio_buttons_bind(bsp_nav_key_t key,
                                const bsp_gpio_button_cfg_t *cfgs, size_t count)
{
    if (key >= BSP_NAV_KEY_COUNT || !cfgs || count == 0) return ESP_ERR_INVALID_ARG;
    nav_key_t *k = &keys[key];
    if (k->count + count > BSP_GPIO_BUTTONS_PER_KEY) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < count; i++) {
        if (!GPIO_IS_VALID_GPIO(cfgs[i].gpio)) return ESP_ERR_INVALID_ARG;
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfgs[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = cfgs[i].pull == BSP_GPIO_PULL_UP ? GPIO_PULLUP_ENABLE
                                                           : GPIO_PULLUP_DISABLE,
            .pull_down_en = cfgs[i].pull == BSP_GPIO_PULL_DOWN ? GPIO_PULLDOWN_ENABLE
                                                               : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) return err;

        gpio_btn_t *b = &k->btns[k->count++];
        b->gpio = cfgs[i].gpio;
        b->active_low = cfgs[i].active_low;
        b->raw_pressed = read_pressed(b);
        b->stable_pressed = b->raw_pressed;   /* 开机时已按住不算一次按下 */
        b->raw_since_us = esp_timer_get_time();
        if (b->stable_pressed)
            k->pressed_cnt++;   /* 聚合计数同步：之后释放成对归零，不产事件也不下溢 */
        ESP_LOGI(TAG, "key %d on GPIO%d (%s, %s)", key, b->gpio,
                 cfgs[i].pull == BSP_GPIO_PULL_UP ? "pull-up" :
                 cfgs[i].pull == BSP_GPIO_PULL_DOWN ? "pull-down" : "floating",
                 b->active_low ? "active-low" : "active-high");
    }
    return ESP_OK;
}

void bsp_gpio_buttons_set_handler(bsp_gpio_button_handler_t fn)
{
    handler = fn;
}

void bsp_gpio_buttons_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (now < next_poll_us) return;
    next_poll_us = now + (int64_t)POLL_PERIOD_MS * 1000;

    for (int key = 0; key < BSP_NAV_KEY_COUNT; key++) {
        nav_key_t *k = &keys[key];
        for (size_t i = 0; i < k->count; i++) {
            gpio_btn_t *b = &k->btns[i];
            bool pressed = read_pressed(b);
            if (pressed != b->raw_pressed) {
                b->raw_pressed = pressed;
                b->raw_since_us = now;
            }
            if (pressed == b->stable_pressed ||
                now - b->raw_since_us < (int64_t)DEBOUNCE_MS * 1000)
                continue;

            b->stable_pressed = pressed;
            if (pressed) {
                /* 任一 GPIO 按下即算该键按下；息屏时的按下只唤醒、吞掉该次按键 */
                if (++k->pressed_cnt == 1) {
                    bool woke = bsp_screen_activity();
                    k->reported = !woke && handler;
                    if (k->reported) handler(key, true);
                }
            } else {
                /* 全部抬起才算抬起 */
                if (--k->pressed_cnt == 0) {
                    if (k->reported) handler(key, false);
                    k->reported = false;
                }
            }
        }
    }
}
