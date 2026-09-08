#include "panel_mgr.h"
#include "ui_anim.h"
#include "titlebar.h"
#include "theme.h"
#include "printer.h"
#include "bsp.h"
#include "lvgl.h"
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
extern panel_def_t panel_display_def;
extern panel_def_t panel_wifi_def;
extern panel_def_t panel_moonraker_def;
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
    &panel_display_def,
    &panel_wifi_def,
    &panel_moonraker_def,
    &panel_printers_def,
    &panel_brightness_def,
};

#define REG_COUNT (sizeof(registry) / sizeof(registry[0]))
#define NAV_DEPTH_MAX 8

static panel_def_t *nav_stack[NAV_DEPTH_MAX];

/* ---------- 编码器焦点导航（lv_group） ---------- */
static lv_group_t *nav_group;      /* 当前可聚焦对象组（绑定编码器 indev） */
static unsigned nav_sig_objs;      /* 上次刷新时的可聚焦对象数（结构变化检测） */
static int nav_top = -1;


/* 焦点目标白名单：lv_obj 默认带 CLICKABLE，不能按 flag 收集（容器/卡片/标签都满足）。
 * 只收交互控件 + 显式标记 USER_1 的操作行。 */
static bool is_nav_target(lv_obj_t *obj)
{
    const lv_obj_class_t *c = lv_obj_get_class(obj);
    if (c == &lv_button_class)  return true;   /* theme_button / 菜单按钮 / 急停重启 / 弹层按钮 */
    if (c == &lv_dropdown_class) return true;  /* 设置页语言/息屏下拉 */
    if (c == &lv_slider_class)  return true;   /* 背光滑块 */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) return true;   /* 显式标记的操作行（wifi/温度/文件等） */
    return false;
}

static lv_obj_tree_walk_res_t collect_cb(lv_obj_t *obj, void *arg)
{
    LV_UNUSED(arg);
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) return LV_OBJ_TREE_WALK_NEXT;
    if (!is_nav_target(obj)) return LV_OBJ_TREE_WALK_NEXT;
    if (lv_obj_check_type(obj, &lv_textarea_class)) return LV_OBJ_TREE_WALK_NEXT;   /* 输入框不聚焦：无键盘，聚焦了是无效块 */
    {
        lv_obj_t *par = lv_obj_get_parent(obj);
        if (par && lv_obj_check_type(par, &lv_dropdownlist_class)) return LV_OBJ_TREE_WALK_NEXT;   /* 下拉列表项不进组：展开态旋转由下拉框自己处理 */
    }
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return LV_OBJ_TREE_WALK_NEXT;
    /* 全屏遮罩层（confirm/keypad 等的 overlay，点击=取消）：不是操作项，整屏高亮是无效块 */
    if (lv_obj_get_width(obj) >= lv_disp_get_hor_res(NULL) &&
        lv_obj_get_height(obj) >= lv_disp_get_ver_res(NULL))
        return LV_OBJ_TREE_WALK_NEXT;
    lv_group_add_obj(nav_group, obj);
    /* 焦点高亮：变色块。用预混色实色（accent 40% + 背景 60%），不用半透明 bg——
     * PARTIAL 渲染下半透明要开 layer，实测在 SPI 屏上出现花屏；实色块视觉相同且无混合开销 */
    lv_obj_set_style_bg_color(obj,
        lv_color_mix(theme_col(THEME_COL_ACCENT), theme_col(THEME_COL_BG), LV_OPA_40),
        LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);   /* 清除旧描边 */
    return LV_OBJ_TREE_WALK_NEXT;
}

static lv_obj_tree_walk_res_t count_cb(lv_obj_t *obj, void *arg)
{
    unsigned *n = arg;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) &&
        is_nav_target(obj)) (*n)++;
    return LV_OBJ_TREE_WALK_NEXT;
}

/* 焦点回调：不自动进编辑模式。
 * 下拉框/滑块聚焦时保持导航态，旋转=正常切换选项（页面能滚到底）；
 * 需调整下拉/滑块时按下编码器按键（LVGL 标准行为）进入编辑，再旋转操作。 */
static void nav_focus_cb(lv_group_t *g)
{
    LV_UNUSED(g);
    lv_group_set_editing(g, false);
}

static void nav_foreach(lv_obj_tree_walk_res_t (*cb)(lv_obj_t *, void *), void *arg)
{
    lv_obj_tree_walk(lv_screen_active(), cb, arg);
    lv_obj_t *top = lv_layer_top();
    bool covered = false;
    uint32_t n = lv_obj_get_child_count(top);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(c, &lv_dropdownlist_class)) continue;   /* 下拉列表整体不进焦点组 */
        if (!covered && lv_obj_get_width(c) >= lv_disp_get_hor_res(NULL) &&
            lv_obj_get_height(c) >= lv_disp_get_ver_res(NULL)) {
            covered = true;   /* 全屏覆盖弹层：只收集其子树，被盖住的 titlebar 等跳过 */
            lv_obj_tree_walk(c, cb, arg);
            continue;
        }
        if (!covered) lv_obj_tree_walk(c, cb, arg);
    }
}

static unsigned nav_obj_count(void)
{
    unsigned n = 0;
    nav_foreach(count_cb, &n);
    return n;
}

static void nav_group_refresh(void)
{
    if (!nav_group) return;
    lv_obj_t *focused = lv_group_get_focused(nav_group);
    lv_group_remove_all_objs(nav_group);
    nav_foreach(collect_cb, NULL);
    if (focused && lv_obj_is_valid(focused) && lv_obj_get_group(focused) == nav_group) {
        lv_group_focus_obj(focused);
    } else {
        /* 旧焦点失效（切页/返回后）：聚焦第一个操作项，避免无高亮需再转一格 */
        if (lv_group_get_focused(nav_group) == NULL)
            lv_group_focus_next(nav_group);
    }
    nav_sig_objs = nav_obj_count();
}

static panel_def_t *find(const char *name)
{
    for (unsigned i = 0; i < REG_COUNT; i++)
        if (strcmp(registry[i]->name, name) == 0) return registry[i];
    return NULL;
}

static void show(panel_def_t *p, int push)
{
    if (p->scr == NULL && p->create)
        p->scr = p->create();   /* 懒加载，之后复用 */
    if (push) ui_screen_push(p->scr);
    else      ui_screen_pop(p->scr);
    titlebar_set(p->title, nav_top > 0);
    titlebar_show_temps(!p->hide_temps);
    if (p->on_show) p->on_show();
    nav_group_refresh();   /* 面板切换后重建焦点组 */
}

void panel_mgr_init(void)
{
    /* 编码器导航组：绑定编码器 indev（desktop 无编码器则跳过绑定，group 仍创建） */
    nav_group = lv_group_create();
    lv_group_set_focus_cb(nav_group, nav_focus_cb);
    lv_indev_t *enc = bsp_encoder_indev();
    if (enc) lv_indev_set_group(enc, nav_group);

    nav_top = 0;
    nav_stack[0] = find("main");
    if (nav_stack[0]->scr == NULL)
        nav_stack[0]->scr = nav_stack[0]->create();
    lv_screen_load(nav_stack[0]->scr);
    titlebar_set(nav_stack[0]->title, 0);
    if (nav_stack[0]->on_show) nav_stack[0]->on_show();
    nav_group_refresh();   /* 首屏收集可聚焦对象 */
}

void panel_mgr_open(const char *name)
{
    panel_def_t *p = find(name);
    if (!p || nav_top >= NAV_DEPTH_MAX - 1) return;
    if (nav_top >= 0 && nav_stack[nav_top] == p) return;   /* 栈顶去重 */
    nav_stack[++nav_top] = p;
    show(p, 1);
}

void panel_mgr_back(void)
{
    if (nav_top <= 0) return;
    nav_top--;
    show(nav_stack[nav_top], 0);
}

void panel_mgr_home(void)
{
    if (nav_top <= 0) return;
    nav_top = 0;
    show(nav_stack[0], 0);
}

void panel_mgr_nav_refresh(void)
{
    if (nav_group) nav_group_refresh();   /* 列表重建后调用：清掉悬空对象、收新对象 */
}

int panel_mgr_depth(void) { return nav_top + 1; }

const char *panel_mgr_current(void)
{
    return nav_top >= 0 ? nav_stack[nav_top]->name : NULL;
}

/* 手动滚动：找最近的可滚动祖先，一次 bounded 滚动让焦点对象完整可见。
 * 不用 lv_obj_scroll_to_view —— 其内部断言路径在本项目对象结构上会卡死 LVGL 任务 */
static void nav_scroll_to(lv_obj_t *o)
{
    lv_obj_update_layout(o);
    lv_obj_t *p = lv_obj_get_parent(o);
    while (p) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) {
            /* 对象相对滚动容器的内容坐标（沿父链累加，再减去滚动偏移） */
            int32_t oy = 0;
            lv_obj_t *cur = o;
            while (cur && cur != p) { oy += lv_obj_get_y(cur); cur = lv_obj_get_parent(cur); }
            oy -= lv_obj_get_scroll_y(p);
            int32_t oh = lv_obj_get_height(o);
            int32_t ph = lv_obj_get_height(p);
            if (oy < 0)               lv_obj_scroll_by_bounded(p, 0, oy, LV_ANIM_OFF);
            else if (oy + oh > ph)    lv_obj_scroll_by_bounded(p, 0, oy + oh - ph, LV_ANIM_OFF);
            break;
        }
        p = lv_obj_get_parent(p);
    }
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
    if (nav_obj_count() != nav_sig_objs) nav_group_refresh();   /* 弹窗开合等结构变化时刷新 */
    /* 编码器不参与容器滚动：焦点对象超出可视区时主动滚容器 */
    lv_obj_t *nf = lv_group_get_focused(nav_group);
    static lv_obj_t *nav_last_focus;
    if (nf && nf != nav_last_focus) {
        nav_last_focus = nf;
        nav_scroll_to(nf);
    }
}
