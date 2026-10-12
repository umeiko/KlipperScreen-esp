#include "gcode_thumb.h"
#include "bsp_caps.h"

#if BSP_HAS_GCODE_THUMB

#include "gcode_thumbnail.h"
#include <stdlib.h>
#include <string.h>

typedef struct { gcode_thumb_state_t state; } thumb_load_t;

gcode_thumb_state_t gcode_thumb_state(lv_obj_t *img)
{
    thumb_load_t *load = img ? lv_obj_get_user_data(img) : NULL;
    return load ? load->state : GCODE_THUMB_FAILED;
}

/* 等比缩放到 max_w/max_h 以内：比它大则缩小，比它小也放大（上限 4x，
 * 避免小缩略图在 Linux 大屏上按原始像素显示得过小）。 */
#ifndef ESP_PLATFORM
static void fit_scale(lv_obj_t *img, int w, int h, int max_w, int max_h)
{
    if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0) return;
    int sw = max_w * 256 / w, sh = max_h * 256 / h;
    int scale = sw < sh ? sw : sh;
    if (scale > 4 * 256) scale = 4 * 256;
    if (scale > 0 && scale != 256) lv_image_set_scale(img, (uint32_t)scale);
}
#endif

/* PNG IHDR（偏移 16，大端）读宽高 */
#ifndef ESP_PLATFORM
static void png_dims(const unsigned char *png, size_t len, int *w, int *h)
{
    *w = *h = 0;
    if (len > 24 && memcmp(png, "\x89PNG\x0d\x0a\x1a\x0a", 8) == 0) {
        *w = (int)(((uint32_t)png[16] << 24) | ((uint32_t)png[17] << 16) |
                   ((uint32_t)png[18] << 8) | (uint32_t)png[19]);
        *h = (int)(((uint32_t)png[20] << 24) | ((uint32_t)png[21] << 16) |
                   ((uint32_t)png[22] << 8) | (uint32_t)png[23]);
    }
}
#endif

#if BSP_HAS_LINUX_HOST

/* ---------- Linux：本地 gcode 同步解析（首选，彩色） ---------- */

typedef struct {
    thumb_load_t    load;     /* First member shared with the state accessor. */
    lv_image_dsc_t  dsc;      /* VARIABLE 源：lodepng 解码器直接读 data/data_size */
    unsigned char  *png;
} thumb_res_t;

static void thumb_free_cb(lv_event_t *e)
{
    thumb_res_t *res = lv_event_get_user_data(e);
    if (!res) return;
    /* RAW variable source is cached under &dsc.  Drop it before freeing the
     * descriptor/blob; otherwise the cache keeps a dangling source key. */
    if (res->dsc.data) lv_image_cache_drop(&res->dsc);
    free(res->png);
    free(res);
}

static lv_obj_t *thumb_create_local(lv_obj_t *parent, const char *gcode_name,
                                    int max_w, int max_h)
{
    char path[600];
    if (!gcode_thumbnail_resolve(gcode_name, path, sizeof(path))) return NULL;

    thumb_res_t *res = calloc(1, sizeof(*res));
    if (!res) return NULL;
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
    res->load.state = GCODE_THUMB_READY;
    lv_obj_set_user_data(img, &res->load);
    lv_image_set_src(img, &res->dsc);
    lv_obj_add_event_cb(img, thumb_free_cb, LV_EVENT_DELETE, res);

    int w, h;
    png_dims(res->png, png_len, &w, &h);
    fit_scale(img, w, h, max_w, max_h);
    return img;
}

#endif /* BSP_HAS_LINUX_HOST */

/* ---------- 远程异步路径（ESP32 唯一路径；Linux 本地 miss 的回退） ----------
 * 共享状态：LVGL 侧的 DELETE 回调与 lv_async_call 投递都在 LVGL 上下文
 * 执行，先后有序不竞争；fetch worker 只碰 png/len 字段。
 * 所有权：t 由"后到者"释放——先 DELETE 后投递则投递回调释放，
 * 先投递后 DELETE 则 DELETE 回调释放（fetch 保证投递恰好一次）。 */
typedef struct {
    thumb_load_t    load;
    lv_obj_t       *img;     /* 占位 image；面板销毁时 DELETE 回调置 NULL */
    lv_image_dsc_t  dsc;
    unsigned char  *blob;    /* ESP32: 灰度 RGB565 像素；Linux: 原始 PNG */
    int             max_w, max_h;
    bool            delivered;
} async_thumb_t;

static void async_thumb_free_cb(lv_event_t *e)
{
    async_thumb_t *t = lv_event_get_user_data(e);
    if (!t) return;
    t->img = NULL;          /* 投递回调看到 NULL 就只释放不触碰对象 */
    if (t->dsc.data) lv_image_cache_drop(&t->dsc);
    free(t->blob);
    t->blob = NULL;
    t->dsc.data = NULL;
    if (t->delivered) free(t);
}

#ifdef ESP_PLATFORM
#include "libs/lodepng/lodepng.h"

/* PNG → 最终显示尺寸的灰度 RGB565。
 *
 * 不把原图交给 LVGL 再用 lv_image_set_scale()：LVGL 9.3 的 RGB565 软件
 * transform 在 ESP32/PSRAM 上会把这类动态图片画成横向噪声和黑块。这里
 * 直接按目标框等比缩放，后续走 1:1 绘制；大缩略图还会显著节省常驻内存。 */
static bool decode_gray(const unsigned char *png, size_t png_len, async_thumb_t *t)
{
    /* LVGL 9.3's bundled lodepng does not return a plain RGBA allocation:
     * it returns an lv_draw_buf_t (cast to unsigned char *) whose data member
     * owns the actual pixels.  Treating the returned pointer as pixels reads
     * the descriptor and then runs past it into unrelated heap memory. */
    lv_draw_buf_t *decoded = NULL;
    unsigned w = 0, h = 0;
    if (lodepng_decode32((unsigned char **)&decoded, &w, &h, png, png_len) != 0) return false;
    if (!decoded || !decoded->data || w == 0 || h == 0 || w > 800 || h > 800) {
        if (decoded) lv_draw_buf_destroy(decoded);
        return false;
    }
    const unsigned char *rgba = decoded->data;

    int sw = t->max_w * 256 / (int)w;
    int sh = t->max_h * 256 / (int)h;
    int scale = sw < sh ? sw : sh;
    if (scale > 4 * 256) scale = 4 * 256;
    if (scale <= 0) { lv_draw_buf_destroy(decoded); return false; }

    unsigned out_w = (unsigned)(((uint64_t)w * (unsigned)scale) / 256U);
    unsigned out_h = (unsigned)(((uint64_t)h * (unsigned)scale) / 256U);
    if (out_w == 0) out_w = 1;
    if (out_h == 0) out_h = 1;

    size_t out_count = (size_t)out_w * out_h;
    uint16_t *px = malloc(out_count * sizeof(*px));
    if (!px) { lv_draw_buf_destroy(decoded); return false; }
    for (unsigned dy = 0; dy < out_h; dy++) {
        unsigned sy = (unsigned)(((uint64_t)dy * h) / out_h);
        for (unsigned dx = 0; dx < out_w; dx++) {
            unsigned sx = (unsigned)(((uint64_t)dx * w) / out_w);
            size_t si = ((size_t)sy * w + sx) * 4;
            unsigned r = rgba[si], g = rgba[si + 1], b = rgba[si + 2];
            unsigned y = (r * 77 + g * 150 + b * 29) >> 8;
            y = (y * rgba[si + 3] + 127) / 255;  /* 透明区与黑底合成 */
            px[(size_t)dy * out_w + dx] =
                (uint16_t)(((y >> 3) << 11) | ((y >> 2) << 5) | (y >> 3));
        }
    }
    lv_draw_buf_destroy(decoded);

    t->blob = (unsigned char *)px;
    t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    t->dsc.header.w = (int32_t)out_w;
    t->dsc.header.h = (int32_t)out_h;
    t->dsc.header.stride = (uint32_t)out_w * 2;
    t->dsc.data_size = (uint32_t)(out_count * sizeof(*px));
    t->dsc.data = t->blob;
    return true;
}
#endif

/* fetch 完成（LVGL 上下文）：png 由本回调负责 free（Linux 彩色路径接管所有权除外） */
static void async_thumb_deliver(unsigned char *png, size_t png_len, void *ud)
{
    async_thumb_t *t = ud;
    t->delivered = true;
    t->load.state = GCODE_THUMB_FAILED;
    if (png && t->img) {
#ifdef ESP_PLATFORM
        if (decode_gray(png, png_len, t)) {
            lv_image_set_src(t->img, &t->dsc);
            t->load.state = GCODE_THUMB_READY;
        }
#else
        /* Linux：保留 PNG 字节，lodepng 解码器绘制时现解（彩色） */
        int w, h;
        png_dims(png, png_len, &w, &h);
        if (w > 0 && h > 0) {
            t->blob = png;
            png = NULL;   /* 所有权移交 blob */
            t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            t->dsc.header.cf = LV_COLOR_FORMAT_RAW;
            t->dsc.header.w = 0;
            t->dsc.header.h = 0;
            t->dsc.data_size = (uint32_t)png_len;
            t->dsc.data = t->blob;
            lv_image_set_src(t->img, &t->dsc);
            fit_scale(t->img, w, h, t->max_w, t->max_h);
            t->load.state = GCODE_THUMB_READY;
        }
#endif
    }
    free(png);
    if (!t->img) {
        free(t->blob);   /* 若 deliver 先于 DELETE 投递成功，blob 在这释放 */
        free(t);
    }
    /* 面板存活时结构体与 blob 由 DELETE 回调释放 */
}

static lv_obj_t *thumb_create_remote(lv_obj_t *parent, const char *gcode_name,
                                     int max_w, int max_h)
{
    async_thumb_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    lv_obj_t *img = lv_image_create(parent);
    t->img = img;
    t->load.state = GCODE_THUMB_LOADING;
    lv_obj_set_user_data(img, &t->load);
    t->max_w = max_w;
    t->max_h = max_h;
    lv_obj_add_event_cb(img, async_thumb_free_cb, LV_EVENT_DELETE, t);
    if (!gcode_thumbnail_fetch_async(gcode_name, async_thumb_deliver, t)) {
        lv_obj_delete(img);     /* 触发 async_thumb_free_cb 断开关联 */
        free(t);
        return NULL;
    }
    return img;
}

lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h)
{
#if BSP_HAS_LINUX_HOST
    /* 本地 gcodes 目录有该文件（打印机就在本机）→ 同步解析彩色缩略图 */
    lv_obj_t *img = thumb_create_local(parent, gcode_name, max_w, max_h);
    if (img) return img;
    /* 本地 miss（连的是远程主机）→ 回退 Moonraker HTTP */
#endif
    return thumb_create_remote(parent, gcode_name, max_w, max_h);
}

#else /* !BSP_HAS_GCODE_THUMB */

gcode_thumb_state_t gcode_thumb_state(lv_obj_t *img)
{
    (void)img;
    return GCODE_THUMB_FAILED;
}

lv_obj_t *gcode_thumb_create(lv_obj_t *parent, const char *gcode_name, int max_w, int max_h)
{
    (void)parent; (void)gcode_name; (void)max_w; (void)max_h;
    return NULL;
}

#endif
