#include "panel_mgr.h"
#include "ui_anim.h"
#include "titlebar.h"
#include "theme.h"
#include "ui_nav.h"
#include "printer.h"
#include <string.h>

/* ---------- 面板注册表（panels 目录下实现，集中声明） ---------- */
extern panel_def_t panel_main_def;
extern panel_def_t panel_job_status_def;
extern panel_def_t panel_temperature_def;
extern panel_def_t panel_move_def;
extern panel_def_t panel_extrude_def;
extern panel_def_t panel_files_def;
extern panel_def_t panel_file_detail_def;
extern panel_def_t panel_settings_def;
extern panel_def_t panel_language_def;
extern panel_def_t panel_display_def;
extern panel_def_t panel_wifi_def;
extern panel_def_t panel_moonraker_def;
extern panel_def_t panel_machine_mode_def;
extern panel_def_t panel_bambu_link_def;
extern panel_def_t panel_bambu_setup_def;
extern panel_def_t panel_printers_def;
extern panel_def_t panel_brightness_def;

static panel_def_t *registry[] = {
    &panel_main_def,
    &panel_job_status_def,
    &panel_temperature_def,
    &panel_move_def,
    &panel_extrude_def,
    &panel_files_def,
    &panel_file_detail_def,
    &panel_settings_def,
    &panel_language_def,
    &panel_display_def,
    &panel_wifi_def,
    &panel_moonraker_def,
    &panel_machine_mode_def,
    &panel_bambu_link_def,
    &panel_bambu_setup_def,
    &panel_printers_def,
    &panel_brightness_def,
};

#define REG_COUNT (sizeof(registry) / sizeof(registry[0]))
#define NAV_DEPTH_MAX 8

static panel_def_t *nav_stack[NAV_DEPTH_MAX];
static int nav_top = -1;

static void ensure_created(panel_def_t *p)
{
    if (p->scr || !p->create) return;
    p->nav_group = ui_nav_group_create();
    ui_nav_prepare_group(p->nav_group);
    p->scr = p->create();
    ui_nav_attach_scope(p->scr, p->nav_group);
}

/* 离开的非主面板不缓存（CYD 等无 PSRAM 机型 17 个面板全常驻必然 OOM）：
   屏幕对象树由转场 auto_del 在动画结束时删除，这里同步回收导航组/scope，
   指针清空后 ensure_created 会在下次进入时重建。 */
static void destroy_left_panel(panel_def_t *p)
{
    if (!p || p == nav_stack[0] || !p->scr) return;
    ui_nav_group_destroy(p->scr, p->nav_group);
    p->scr = NULL;
    p->nav_group = NULL;
}

static panel_def_t *find(const char *name)
{
    for (unsigned i = 0; i < REG_COUNT; i++)
        if (strcmp(registry[i]->name, name) == 0) return registry[i];
    return NULL;
}

static void show(panel_def_t *p, int push, panel_def_t *leaving)
{
    ensure_created(p);          /* 懒加载：常驻的只有主面板和当前面板 */
    bool del_prev = leaving && leaving != nav_stack[0] && leaving->scr;
    if (push) ui_screen_push(p->scr, del_prev);
    else      ui_screen_pop(p->scr, del_prev);
    if (del_prev) destroy_left_panel(leaving);   /* 对象树由 LVGL 在转场结束后删除 */
    const char *title = (ui_scale() < 1.0f && p->title_s) ? p->title_s : p->title;
    titlebar_set(title, nav_top > 0);
    /* 小屏（scale<1）标题位窄，子面板的温度让位给标题，只在主面板（时钟位）显示 */
    titlebar_show_temps(!p->hide_temps && (ui_scale() >= 1.0f || nav_top == 0));
    ui_nav_activate(p->nav_group);
    ui_nav_set_global_obj(titlebar_back_button(), nav_top > 0);
    if (p->on_show) p->on_show();
    ui_nav_refocus_visible(p->nav_group);
}

void panel_mgr_init(void)
{
    nav_top = 0;
    nav_stack[0] = find("main");
    ensure_created(nav_stack[0]);
    lv_screen_load(nav_stack[0]->scr);
    titlebar_set(nav_stack[0]->title, 0);
    ui_nav_activate(nav_stack[0]->nav_group);
    ui_nav_set_global_obj(titlebar_back_button(), false);
    if (nav_stack[0]->on_show) nav_stack[0]->on_show();
    ui_nav_refocus_visible(nav_stack[0]->nav_group);
}

void panel_mgr_open(const char *name)
{
    panel_def_t *p = find(name);
    if (!p || nav_top >= NAV_DEPTH_MAX - 1) return;
    if (nav_top >= 0 && nav_stack[nav_top] == p) return;   /* 栈顶去重 */
    panel_def_t *leaving = nav_top >= 0 ? nav_stack[nav_top] : NULL;
    nav_stack[++nav_top] = p;
    show(p, 1, leaving);
}

void panel_mgr_back(void)
{
    if (nav_top <= 0) return;
    panel_def_t *leaving = nav_stack[nav_top--];
    show(nav_stack[nav_top], 0, leaving);
}

void panel_mgr_home(void)
{
    if (nav_top <= 0) return;
    panel_def_t *leaving = nav_stack[nav_top];
    nav_top = 0;
    show(nav_stack[0], 0, leaving);
}

int panel_mgr_depth(void) { return nav_top + 1; }

const char *panel_mgr_current(void)
{
    return nav_top >= 0 ? nav_stack[nav_top]->name : NULL;
}

void panel_mgr_tick(void)
{
    /* klippy 报错（点动超程/未归零等 "!!" 响应行）→ 弹 toast */
    char err[96];
    if (printer_take_error(err, sizeof(err)))
        ui_toast(err, THEME_COL_ERROR);

    if (nav_top >= 0 && nav_stack[nav_top]->on_tick)
        nav_stack[nav_top]->on_tick();
    titlebar_tick();
}
