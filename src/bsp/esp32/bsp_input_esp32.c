#include "bsp.h"
#include "bsp_rotary_encoder.h"
#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "bsp_input";

void bsp_input_init(void)
{
#if CONFIG_INPUT_ROTARY_ENCODER
    const bsp_rotary_encoder_config_t config = {
        .gpio_a = CONFIG_INPUT_ROTARY_GPIO_A,
        .gpio_b = CONFIG_INPUT_ROTARY_GPIO_B,
        .gpio_button = CONFIG_INPUT_ROTARY_GPIO_BUTTON,
        .counts_per_detent = CONFIG_INPUT_ROTARY_COUNTS_PER_DETENT,
        .button_debounce_ms = CONFIG_INPUT_ROTARY_BUTTON_DEBOUNCE_MS,
        .glitch_filter_ns = CONFIG_INPUT_ROTARY_GLITCH_FILTER_NS,
#ifdef CONFIG_INPUT_ROTARY_PHASE_PULLUPS
        .phase_pullups = true,
#else
        .phase_pullups = false,
#endif
#ifdef CONFIG_INPUT_ROTARY_REVERSE
        .reverse = true,
#else
        .reverse = false,
#endif
#ifdef CONFIG_INPUT_ROTARY_BUTTON_ACTIVE_LOW
        .button_active_low = true,
#else
        .button_active_low = false,
#endif
    };
    esp_err_t err = bsp_rotary_encoder_create(&config, bsp_get_display(), NULL);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "optional rotary encoder init failed: %s", esp_err_to_name(err));
#else
    ESP_LOGD(TAG, "optional rotary encoder disabled");
#endif
}
