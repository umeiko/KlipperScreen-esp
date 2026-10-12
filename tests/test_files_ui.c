/* Reuse the real model and UI; fake only delivery timing. No printer/network writes. */
#include <SDL.h>
#undef main
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "moonraker_client.h"
#include "bsp.h"
#include "ui_app.h"
static void (*reply_cb)(char *, void *);
static void *reply_ud;
static bool fake_rpc(const char *method, const char *params, void (*cb)(char *, void *), void *ud)
{
    assert(strcmp(method, "server.files.list") == 0);
    (void)params; reply_cb = cb; reply_ud = ud; return true;
}
#define moonraker_rpc fake_rpc
#include "../src/core/printer_model.c"
#undef moonraker_rpc
#define refresh files_ui_refresh
#include "../src/ui/panels/panel_files.c"
#undef refresh

/* Measure/bound the real LVGL allocator, not estimated widget sizes. */
typedef union { size_t size; long double align; } header_t;
void *__real_lv_malloc_core(size_t);
void *__real_lv_realloc_core(void *, size_t);
void __real_lv_free_core(void *);
static size_t ui_live, ui_base, ui_peak, limit = SIZE_MAX;
static const char *phase;
static void used(size_t n) {
    ui_live += n; if (ui_live > ui_peak) ui_peak = ui_live;
    if (ui_live > limit) fprintf(stderr, "phase=%s alloc=%zu base=%zu live=%zu limit=%zu\n", phase, n, ui_base, ui_live, limit);
    assert(ui_live <= limit);
}
void *__wrap_lv_malloc_core(size_t n)
{
    header_t *h = __real_lv_malloc_core(sizeof(*h) + n); assert(h);
    h->size = n; used(n); return h + 1;
}
void __wrap_lv_free_core(void *p)
{
    if (!p) return;
    header_t *h = (header_t *)p - 1; ui_live -= h->size; __real_lv_free_core(h);
}
void *__wrap_lv_realloc_core(void *p, size_t n)
{
    if (!p) return __wrap_lv_malloc_core(n);
    if (!n) { __wrap_lv_free_core(p); return NULL; }
    header_t *h = (header_t *)p - 1; size_t before = h->size;
    h = __real_lv_realloc_core(h, sizeof(*h) + n); assert(h);
    h->size = n; ui_live -= before; used(n); return h + 1;
}
static char *fixture(unsigned n)
{
    char *json = malloc(n * 128u + 3); assert(json);
    size_t at = 0; json[at++] = '[';
    for (unsigned i = 0; i < n; i++)
        at += sprintf(json + at, "%s{\"path\":\"folder/part_%05u_long_name.gcode\",\"size\":12345678,\"modified\":1756800000}", i ? "," : "", i);
    json[at++] = ']'; json[at] = 0; return json;
}
static void deliver(unsigned n)
{
    reply_cb(fixture(n), reply_ud);
    SDL_Delay(20); /* At least one render tick; real HTTP pages take much longer. */
    lv_timer_handler(); /* Drain normal deferred navigation/refresh work between requests. */
    lv_obj_update_layout(lv_screen_active());
}
static void settle(void)
{
    uint32_t start = SDL_GetTicks();
    do { lv_timer_handler(); SDL_Delay(5); } while (SDL_GetTicks() - start < 250);
}
int main(void)
{
    bsp_init(); bsp_input_init(); ui_app_create(); settle();
    ui_base = ui_live; ui_peak = ui_live; limit = ui_base + 50 * 1024;
    M.state = PRINTER_STATE_STANDBY;
    phase = "initial files entry";
    ui_app_open("files"); assert(loading); deliver(1000); settle();
    assert(current.total == 1000 && current.count == 8);
    assert(lv_obj_get_child_count(list) == 8);
    lv_obj_send_event(next_btn, LV_EVENT_CLICKED, NULL); assert(offset == 8 && loading); deliver(1000);
    lv_obj_send_event(prev_btn, LV_EVENT_CLICKED, NULL); assert(offset == 0 && loading); deliver(1000);
    settle();
    size_t stable = ui_live;
    phase = "paging";
    for (unsigned page = 1; page < 125; page++) {
        request_page(page * 8); assert(loading); deliver(1000);
        assert(current.count == 8 && lv_obj_get_child_count(list) == 8);
        char expected[96]; snprintf(expected, sizeof(expected), "folder/part_%05u_long_name.gcode", page * 8);
        assert(strcmp(current.files[0].name, expected) == 0); /* Label must not alter print path. */
    }
    settle();
    fprintf(stderr, "steady-state delta=%lld peak=%zu\n", (long long)ui_live - (long long)stable, ui_peak-ui_base);
    assert(ui_live <= stable + 1024);
    request_page(992); deliver(999); assert(current.count == 7);
    request_page(0); reply_cb(NULL, reply_ud); assert(!loading);
    assert(!lv_obj_has_state(refresh_btn, LV_STATE_DISABLED));
    request_page(0); files_req.start_ms = lv_tick_get() - FILES_REQ_TIMEOUT_MS - 1;
    printer_files_poll(); assert(!loading && !files_req.in_flight);
    request_page(0);
    void (*late_cb)(char *, void *) = reply_cb; void *late_ud = reply_ud;
    phase = "leave files";
    ui_app_open("display"); settle(); assert(!files_req.in_flight);
    ui_app_open("files"); assert(loading);
    late_cb(fixture(2), late_ud); assert(loading); /* Does not complete a newer request. */
    deliver(0); assert(!loading && current.count == 0);
    for (int i = 0; i < 10; i++) {
        ui_app_open("display"); settle(); ui_app_open("files"); deliver(1000); settle();
    }
    printf("PASS: 125 pages/1000 files, fixed 8 rows, LVGL peak increment=%zu (<50KiB), retained delta=%lld\n",
        ui_peak - ui_base, (long long)ui_live - (long long)stable);
    puts("PASS: empty, error, timeout without status feed, navigation, late-reply rejection, repeated reopen, unchanged file paths");
    return 0;
}
