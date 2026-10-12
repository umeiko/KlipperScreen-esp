#pragma once
/*
 * gcode 缩略图挂件：为 gcode 文件创建 lv_image（BSP_HAS_GCODE_THUMB 平台；
 * 其余平台与无缩略图文件返回 NULL，调用方按普通文本布局即可）。
 * Linux 优先本地解析（彩色）；本地 miss（连远程主机）与 ESP32 走
 * Moonraker HTTP 异步拉取（ESP32 转灰度省内存），字节流/结构体随
 * LV_EVENT_DELETE 与投递回调的"后到者"释放。缩略图等比适配给定区域。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* max_w/max_h：允许占用的最大像素；缩略图比它小则原大显示，比它大则等比缩小。 */
lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h);

typedef enum {
    GCODE_THUMB_LOADING,
    GCODE_THUMB_READY,
    GCODE_THUMB_FAILED,
} gcode_thumb_state_t;
/* Allows a visible panel to retry a failed asynchronous fetch without treating
 * a still-running request as failure. Call only for a live thumbnail widget. */
gcode_thumb_state_t gcode_thumb_state(lv_obj_t *img);

#ifdef __cplusplus
}
#endif
