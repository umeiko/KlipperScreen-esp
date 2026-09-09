#include "bsp_screen_power.h"

#include "bsp.h"

typedef struct {
    bsp_backlight_apply_cb_t apply_backlight;
    bsp_screen_now_ms_cb_t now_ms;
    uint64_t last_activity_ms;
    uint32_t timeout_s;
    int brightness_pct;
    bool off;
    bool initialized;
} screen_power_state_t;

static screen_power_state_t state = {
    .brightness_pct = 100,
};

static uint64_t now_ms(void)
{
    return state.now_ms ? state.now_ms() : 0;
}

static void apply_effective_brightness(void)
{
    if (state.apply_backlight)
        state.apply_backlight(state.off ? 0 : state.brightness_pct);
}

void bsp_screen_power_init(bsp_backlight_apply_cb_t apply_backlight,
                           bsp_screen_now_ms_cb_t clock_ms)
{
    state.apply_backlight = apply_backlight;
    state.now_ms = clock_ms;
    state.last_activity_ms = now_ms();
    state.timeout_s = 0;
    state.brightness_pct = 100;
    state.off = false;
    state.initialized = true;
    apply_effective_brightness();
}

void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    state.brightness_pct = pct;
    if (state.initialized) apply_effective_brightness();
}

void bsp_set_screen_timeout(uint32_t sec)
{
    state.timeout_s = sec;
    state.last_activity_ms = now_ms();
}

bool bsp_screen_activity(void)
{
    bool woke = state.off;
    state.last_activity_ms = now_ms();
    if (state.off) {
        state.off = false;
        apply_effective_brightness();
    }
    return woke;
}

void bsp_screen_off(void)
{
    if (state.off) return;
    state.off = true;
    apply_effective_brightness();
}

void bsp_screen_wake(void)
{
    (void)bsp_screen_activity();
}

void bsp_screen_toggle(void)
{
    if (state.off)
        bsp_screen_wake();
    else
        bsp_screen_off();
}

bool bsp_screen_is_off(void)
{
    return state.off;
}

void bsp_screen_power_poll(void)
{
    if (!state.initialized || state.off || state.timeout_s == 0) return;
    uint64_t elapsed_ms = now_ms() - state.last_activity_ms;
    if (elapsed_ms >= (uint64_t)state.timeout_s * 1000)
        bsp_screen_off();
}
