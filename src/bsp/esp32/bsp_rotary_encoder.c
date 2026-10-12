#include "bsp_rotary_encoder.h"
#include "bsp.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#define PCNT_LIMIT 30000
#else
/* 500Hz 能覆盖手动快速旋转时的 EC11 四相转移；10ms/100Hz 会在
   相邻两次采样间跨过多个状态，转移表无法还原已丢失的边沿。 */
#define QDEC_POLL_US 2000
#endif

typedef struct {
#if SOC_PCNT_SUPPORTED
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channel_a;
    pcnt_channel_handle_t channel_b;
#else
    int gpio_a;                 /* 软件解码后端：A/B 相 GPIO */
    int gpio_b;
    esp_timer_handle_t qdec_timer;  /* 2ms 轮询定时器 */
    volatile int qdec_state;    /* 轮询里的 4bit 转移索引 */
    volatile int sw_count;      /* 轮询累计的正交计数（等效 PCNT count） */
#endif
    int last_count;
    int remainder;
    int counts_per_detent;
    int gpio_button;
    int debounce_ms;
    int64_t raw_changed_us;
    bool reverse;
    bool active_low;
    bool raw_pressed;
    bool stable_pressed;
    bool swallow_button;
} rotary_ctx_t;

static const char *TAG = "rotary_encoder";

#if !SOC_PCNT_SUPPORTED
/* 无 PCNT 芯片（ESP32-C3）的软件正交解码：2ms 定时器轮询 A/B 电平，
   4bit（旧2位+新2位）状态转移表查 ±1/0。抖动造成的来回跳变在
   表内净值天然为 0，无需定时消抖（glitch_filter_ns 配置在本后端忽略）。
   不用 GPIO 中断：悬浮/噪声输入不会形成中断风暴。esp_timer 回调已由
   IDF 高优先级 timer task 派发，丢脉冲的主因是采样率而不是任务优先级。 */
static const int8_t qdec_table[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
};

static void qdec_poll(void *arg)
{
    rotary_ctx_t *ctx = arg;
    int level = (gpio_get_level(ctx->gpio_a) << 1) | gpio_get_level(ctx->gpio_b);
    ctx->qdec_state = ((ctx->qdec_state << 2) | level) & 0xF;
    ctx->sw_count += qdec_table[ctx->qdec_state];
}
#endif

static void rotary_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    rotary_ctx_t *ctx = lv_indev_get_user_data(indev);
    int count = ctx->last_count;
#if SOC_PCNT_SUPPORTED
    if (pcnt_unit_get_count(ctx->unit, &count) == ESP_OK) {
        int delta = count - ctx->last_count;
        ctx->last_count = count;
        if (ctx->reverse) delta = -delta;
        ctx->remainder += delta;
    }
#else
    count = ctx->sw_count;
    {
        int delta = count - ctx->last_count;
        ctx->last_count = count;
        if (ctx->reverse) delta = -delta;
        ctx->remainder += delta;
    }
#endif

    int detents = ctx->remainder / ctx->counts_per_detent;
    ctx->remainder -= detents * ctx->counts_per_detent;
    if (detents > INT16_MAX) detents = INT16_MAX;
    if (detents < INT16_MIN) detents = INT16_MIN;

    int64_t now = esp_timer_get_time();
    bool raw_pressed = gpio_get_level(ctx->gpio_button) == (ctx->active_low ? 0 : 1);
    if (raw_pressed != ctx->raw_pressed) {
        ctx->raw_pressed = raw_pressed;
        ctx->raw_changed_us = now;
    } else if (raw_pressed != ctx->stable_pressed &&
               now - ctx->raw_changed_us >= (int64_t)ctx->debounce_ms * 1000) {
        ctx->stable_pressed = raw_pressed;
    }

    data->enc_diff = detents;
    data->state = ctx->stable_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    if (detents != 0 || ctx->stable_pressed) {
        if (bsp_screen_activity()) {
            data->enc_diff = 0;
            if (ctx->stable_pressed) ctx->swallow_button = true;
        }
    }
    if (!ctx->stable_pressed)
        ctx->swallow_button = false;
    if (ctx->swallow_button)
        data->state = LV_INDEV_STATE_RELEASED;
}

int bsp_rotary_encoder_get_counts(lv_indev_t *indev)
{
    rotary_ctx_t *ctx = indev ? lv_indev_get_user_data(indev) : NULL;
    return ctx ? ctx->counts_per_detent : 0;
}

bool bsp_rotary_encoder_set_counts(lv_indev_t *indev, int counts)
{
    rotary_ctx_t *ctx = indev ? lv_indev_get_user_data(indev) : NULL;
    if (!ctx || counts < 1 || counts > 8) return false;
    int count;
#if SOC_PCNT_SUPPORTED
    if (pcnt_unit_get_count(ctx->unit, &count) != ESP_OK) return false;
#else
    count = ctx->sw_count;
#endif
    ctx->last_count = count;
    ctx->remainder = 0;
    ctx->counts_per_detent = counts;
    return true;
}

static void cleanup(rotary_ctx_t *ctx, bool enabled, bool started)
{
    if (!ctx) return;
#if SOC_PCNT_SUPPORTED
    if (started) pcnt_unit_stop(ctx->unit);
    if (enabled) pcnt_unit_disable(ctx->unit);
    if (ctx->channel_b) pcnt_del_channel(ctx->channel_b);
    if (ctx->channel_a) pcnt_del_channel(ctx->channel_a);
    if (ctx->unit) pcnt_del_unit(ctx->unit);
#else
    (void)enabled;
    if (started && ctx->qdec_timer) esp_timer_stop(ctx->qdec_timer);
    if (ctx->qdec_timer) esp_timer_delete(ctx->qdec_timer);
#endif
    free(ctx);
}

esp_err_t bsp_rotary_encoder_create(const bsp_rotary_encoder_config_t *config,
                                    lv_display_t *display,
                                    lv_indev_t **out_indev)
{
    if (!config || !display || !GPIO_IS_VALID_GPIO(config->gpio_a) ||
        !GPIO_IS_VALID_GPIO(config->gpio_b) || !GPIO_IS_VALID_GPIO(config->gpio_button) ||
        config->gpio_a == config->gpio_b || config->gpio_a == config->gpio_button ||
        config->gpio_b == config->gpio_button || config->counts_per_detent < 1 ||
        config->button_debounce_ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    rotary_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return ESP_ERR_NO_MEM;
    ctx->counts_per_detent = config->counts_per_detent;
    ctx->gpio_button = config->gpio_button;
    ctx->debounce_ms = config->button_debounce_ms;
    ctx->reverse = config->reverse;
    ctx->active_low = config->button_active_low;

    bool enabled = false;
    bool started = false;
    esp_err_t err;
#if SOC_PCNT_SUPPORTED
    pcnt_unit_config_t unit_config = {
        .high_limit = PCNT_LIMIT,
        .low_limit = -PCNT_LIMIT,
        .flags.accum_count = 1,
    };
    if ((err = pcnt_new_unit(&unit_config, &ctx->unit)) != ESP_OK) goto fail;

    if (config->glitch_filter_ns > 0) {
        pcnt_glitch_filter_config_t filter = { .max_glitch_ns = config->glitch_filter_ns };
        if ((err = pcnt_unit_set_glitch_filter(ctx->unit, &filter)) != ESP_OK) goto fail;
    }

    pcnt_chan_config_t channel_a_config = {
        .edge_gpio_num = config->gpio_a,
        .level_gpio_num = config->gpio_b,
    };
    pcnt_chan_config_t channel_b_config = {
        .edge_gpio_num = config->gpio_b,
        .level_gpio_num = config->gpio_a,
    };
    if ((err = pcnt_new_channel(ctx->unit, &channel_a_config, &ctx->channel_a)) != ESP_OK) goto fail;
    if ((err = pcnt_new_channel(ctx->unit, &channel_b_config, &ctx->channel_b)) != ESP_OK) goto fail;
    /* The IDF PCNT driver enables pull-ups while routing its GPIOs. Apply the
       configured final mode afterwards so external-resistor modules can opt out. */
    gpio_pull_mode_t phase_pull = config->phase_pullups ? GPIO_PULLUP_ONLY : GPIO_FLOATING;
    if ((err = gpio_set_pull_mode(config->gpio_a, phase_pull)) != ESP_OK) goto fail;
    if ((err = gpio_set_pull_mode(config->gpio_b, phase_pull)) != ESP_OK) goto fail;
    if ((err = pcnt_channel_set_edge_action(ctx->channel_a,
                                             PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                             PCNT_CHANNEL_EDGE_ACTION_INCREASE)) != ESP_OK) goto fail;
    if ((err = pcnt_channel_set_level_action(ctx->channel_a,
                                              PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                              PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK) goto fail;
    if ((err = pcnt_channel_set_edge_action(ctx->channel_b,
                                             PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                             PCNT_CHANNEL_EDGE_ACTION_DECREASE)) != ESP_OK) goto fail;
    if ((err = pcnt_channel_set_level_action(ctx->channel_b,
                                              PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                              PCNT_CHANNEL_LEVEL_ACTION_INVERSE)) != ESP_OK) goto fail;

    if ((err = pcnt_unit_add_watch_point(ctx->unit, PCNT_LIMIT)) != ESP_OK) goto fail;
    if ((err = pcnt_unit_add_watch_point(ctx->unit, -PCNT_LIMIT)) != ESP_OK) goto fail;
    if ((err = pcnt_unit_enable(ctx->unit)) != ESP_OK) goto fail;
    enabled = true;
    if ((err = pcnt_unit_clear_count(ctx->unit)) != ESP_OK) goto fail;
    if ((err = pcnt_unit_start(ctx->unit)) != ESP_OK) goto fail;
    started = true;
#else
    /* 软件解码后端（ESP32-C3 无 PCNT）：A/B 相输入 + 2ms 定时轮询 */
    ctx->gpio_a = config->gpio_a;
    ctx->gpio_b = config->gpio_b;
    gpio_config_t phase_config = {
        .pin_bit_mask = (1ULL << config->gpio_a) | (1ULL << config->gpio_b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->phase_pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&phase_config)) != ESP_OK) goto fail;
    int level = (gpio_get_level(config->gpio_a) << 1) | gpio_get_level(config->gpio_b);
    ctx->qdec_state = (level << 2) | level;
    ctx->sw_count = 0;
    const esp_timer_create_args_t qdec_timer_args = {
        .callback = qdec_poll,
        .arg = ctx,
        .name = "qdec",
    };
    if ((err = esp_timer_create(&qdec_timer_args, &ctx->qdec_timer)) != ESP_OK) goto fail;
    if ((err = esp_timer_start_periodic(ctx->qdec_timer, QDEC_POLL_US)) != ESP_OK) goto fail;
    started = true;
#endif

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << config->gpio_button,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->button_active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = config->button_active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if ((err = gpio_config(&button_config)) != ESP_OK) goto fail;
    ctx->raw_pressed = gpio_get_level(ctx->gpio_button) == (ctx->active_low ? 0 : 1);
    ctx->stable_pressed = ctx->raw_pressed;
    ctx->raw_changed_us = esp_timer_get_time();

    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, rotary_read_cb);
    lv_indev_set_user_data(indev, ctx);
    lv_indev_set_display(indev, display);
    if (out_indev) *out_indev = indev;

    ESP_LOGI(TAG, "encoder ready (%s): A=%d B=%d button=%d counts/detent=%d%s",
#if SOC_PCNT_SUPPORTED
             "pcnt",
#else
             "timer-poll",
#endif
             config->gpio_a, config->gpio_b, config->gpio_button,
             config->counts_per_detent, config->reverse ? " reversed" : "");
    return ESP_OK;

fail:
    cleanup(ctx, enabled, started);
    return err;
}
