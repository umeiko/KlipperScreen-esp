#include "bsp_sleep_button.h"

#include "bsp.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define POLL_PERIOD_MS   10
#define DEBOUNCE_MS      30
#define MAX_BUTTONS      8

static const char *TAG = "sleep_button";

typedef struct {
    int     gpio;
    bool    active_low;
    bool    stable_pressed;   /* 消抖后的稳定状态 */
    bool    raw_pressed;      /* 上一次采样到的原始状态 */
    int64_t raw_since_us;     /* 原始状态最近一次变化的时间 */
} button_state_t;

static button_state_t buttons[MAX_BUTTONS];
static size_t         button_count;
static int64_t        next_poll_us;

static bool read_pressed(const button_state_t *b)
{
    return gpio_get_level(b->gpio) == (b->active_low ? 0 : 1);
}

void bsp_sleep_button_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (now < next_poll_us) return;
    next_poll_us = now + (int64_t)POLL_PERIOD_MS * 1000;

    for (size_t i = 0; i < button_count; i++) {
        button_state_t *b = &buttons[i];
        bool pressed = read_pressed(b);
        if (pressed != b->raw_pressed) {
            b->raw_pressed = pressed;
            b->raw_since_us = now;
        }
        if (pressed != b->stable_pressed &&
            now - b->raw_since_us >= (int64_t)DEBOUNCE_MS * 1000) {
            b->stable_pressed = pressed;
            if (pressed) {
                ESP_LOGI(TAG, "GPIO%d pressed: toggle screen", b->gpio);
                bsp_screen_toggle();
            }
        }
    }
}

esp_err_t bsp_sleep_button_init(const bsp_sleep_button_cfg_t *cfgs, size_t count)
{
    if (!cfgs || count == 0) return ESP_ERR_INVALID_ARG;
    if (button_count + count > MAX_BUTTONS) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < count; i++) {
        if (!GPIO_IS_VALID_GPIO(cfgs[i].gpio)) return ESP_ERR_INVALID_ARG;
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfgs[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = cfgs[i].active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = cfgs[i].active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) return err;

        button_state_t *b = &buttons[button_count++];
        b->gpio = cfgs[i].gpio;
        b->active_low = cfgs[i].active_low;
        b->raw_pressed = read_pressed(b);
        b->stable_pressed = b->raw_pressed;  /* 开机时已按住不算一次按下 */
        b->raw_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "sleep button on GPIO%d (%s)", b->gpio,
                 b->active_low ? "active-low" : "active-high");
    }

    return ESP_OK;
}
