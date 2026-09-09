#pragma once

/*
 * Shared screen/backlight state machine.
 *
 * The UI owns a logical brightness in the 0..100 range.  A board supplies
 * only the final hardware mapping (PWM polarity/curve, backlight IC, etc.).
 * Call poll from the same task that services LVGL input so activity, timeout
 * and explicit screen-button commands have one owner.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bsp_backlight_apply_cb_t)(int percent);
typedef uint64_t (*bsp_screen_now_ms_cb_t)(void);

void bsp_screen_power_init(bsp_backlight_apply_cb_t apply_backlight,
                           bsp_screen_now_ms_cb_t now_ms);
void bsp_screen_power_poll(void);

#ifdef __cplusplus
}
#endif
