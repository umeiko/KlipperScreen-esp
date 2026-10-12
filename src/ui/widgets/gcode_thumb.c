#include "gcode_thumb.h"
#include "bsp_caps.h"

#if BSP_HAS_LINUX_HOST

#include "gcode_thumbnail.h"
#include <stdlib.h>

typedef struct {
    lv_image_dsc_t  dsc;      /* VARIABLE 源：lodepng 解码器直接读 data/data_size */
    unsigned char  *png;
} thumb_res_t;

static void thumb_free_cb(lv_event_t *e)
{
    thumb_res_t *res = lv_event_get_user_data(e);
    if (!res) return;
    free(res->png);
    free(res);
}

lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h)
{
    char path[600];
    if (!gcode_thumbnail_resolve(gcode_name, path, sizeof(path))) return NULL;

    thumb_res_t *res = malloc(sizeof(thumb_res_t));
    if (!res) return NULL;
    res->png = NULL;
    size_t png_len = 0;
    if (!gcode_thumbnail_load(path, &res->png, &png_len)) {
        free(res);
        return NULL;
    }

    res->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    res->dsc.header.cf = LV_COLOR_FORMAT_RAW;   /* 解码器只认 magic+尺寸，由 lodepng 接管 */
    res->dsc.header.w = 0;
    res->dsc.header.h = 0;
    res->dsc.data_size = (uint32_t)png_len;
    res->dsc.data = res->png;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &res->dsc);
    lv_obj_add_event_cb(img, thumb_free_cb, LV_EVENT_DELETE, res);

    /* 等比缩小到 max_w/max_h 以内；尺寸直接读 PNG IHDR（偏移 16，大端） */
    if (png_len > 24) {
        int w = (res->png[16] << 24) | (res->png[17] << 16) | (res->png[18] << 8) | res->png[19];
        int h = (res->png[20] << 24) | (res->png[21] << 16) | (res->png[22] << 8) | res->png[23];
        if (w > 0 && h > 0 && (w > max_w || h > max_h)) {
            int sw = max_w * 256 / w, sh = max_h * 256 / h;
            int scale = sw < sh ? sw : sh;
            if (scale < 256 && scale > 0) lv_image_set_scale(img, (uint32_t)scale);
        }
    }
    return img;
}

#else

lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h)
{
    (void)parent; (void)gcode_name; (void)max_w; (void)max_h;
    return NULL;
}

#endif
