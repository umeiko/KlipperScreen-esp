#pragma once
/*
 * gcode 缩略图挂件：为本地 gcode 文件创建居中的 lv_image（Linux 上位机；
 * 其余平台与无缩略图文件返回 NULL，调用方按普通文本布局即可）。
 * PNG 字节流与描述符随图像对象删除自动释放（挂在 LV_EVENT_DELETE 上）。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* max_w/max_h：允许占用的最大像素；缩略图比它小则原大显示，比它大则等比缩小。 */
lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h);

#ifdef __cplusplus
}
#endif
