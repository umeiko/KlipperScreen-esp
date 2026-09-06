#include "toggle_group.h"
#include "../theme.h"
#include <stdlib.h>

typedef struct {
    lv_obj_t **btns;
    int count;
    int selected;
    toggle_group_cb_t cb;
    void *ud;
} tg_ctx_t;

static void refresh(tg_ctx_t *ctx)
{
    for (int i = 0; i < ctx->count; i++) {
        lv_obj_set_style_bg_color(ctx->btns[i],
            theme_col(i == ctx->selected ? THEME_COL_ACCENT : THEME_COL_SURFACE2), 0);
    }
}

static void on_click(lv_event_t *e)
{
    tg_ctx_t *ctx = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx == ctx->selected) return;
    ctx->selected = idx;
    refresh(ctx);
    if (ctx->cb) ctx->cb(idx, ctx->ud);
}

lv_obj_t *toggle_group_create(lv_obj_t *parent, const char **items, int count,
                              int selected, toggle_group_cb_t cb, void *user_data)
{
    tg_ctx_t *ctx = malloc(sizeof(tg_ctx_t));
    ctx->btns = malloc(sizeof(lv_obj_t *) * count);
    ctx->count = count;
    ctx->selected = selected;
    ctx->cb = cb;
    ctx->ud = user_data;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, ui_px(4), 0);

    for (int i = 0; i < count; i++) {
        lv_obj_t *b = theme_button(row, NULL, items[i], 0);
        lv_obj_set_height(b, ui_px(28));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, ctx);
        ctx->btns[i] = b;
    }
    refresh(ctx);
    return row;
}

int toggle_group_get(lv_obj_t *group)
{
    LV_UNUSED(group);
    return -1;   /* 状态由回调驱动，暂不需要反向查询 */
}
