/* 能力注册：上层不判断板名，不改第三方驱动私有字段或 MADCTL 缓存。
 * 未接入的后端保持不支持，默认不改变任何既有板型的颜色或性能。 */
#include "bsp.h"

static bool native_bgr;
static bsp_color_order_t selected;
static bsp_color_order_apply_t apply_order;

void bsp_disp_color_order_register(bool default_bgr, bsp_color_order_apply_t apply)
{
    native_bgr = default_bgr;
    selected = BSP_COLOR_ORDER_DEFAULT;
    apply_order = apply;
}

bool bsp_disp_can_color_order(void) { return apply_order != NULL; }
bsp_color_order_t bsp_disp_get_color_order(void) { return selected; }

bool bsp_disp_set_color_order(bsp_color_order_t order)
{
    if (!apply_order || order < BSP_COLOR_ORDER_DEFAULT || order > BSP_COLOR_ORDER_BGR)
        return false;
    bool bgr = order == BSP_COLOR_ORDER_DEFAULT ? native_bgr : order == BSP_COLOR_ORDER_BGR;
    if (!apply_order(bgr)) return false;
    selected = order;
    return true;
}
