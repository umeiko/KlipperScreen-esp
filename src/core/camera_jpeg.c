#include "camera_jpeg.h"

#include <stdlib.h>
#include <string.h>

#if LV_USE_TJPGD
#ifdef ESP_PLATFORM
#include "jpeg_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "libs/tjpgd/tjpgd.h"

/* Private DCT-downscaling variant; renamed symbols coexist with LVGL's TJPGD. */
#undef JD_USE_SCALE
#define JD_USE_SCALE 1
#define jd_prepare camera_jd_prepare
#define jd_decomp camera_jd_decomp
#define jd_mcu_load camera_jd_mcu_load
#define jd_mcu_output camera_jd_mcu_output
#define jd_restart camera_jd_restart
#include "libs/tjpgd/tjpgd.c"

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} jpeg_input_t;

typedef struct {
    jpeg_input_t *input;
    uint16_t *pixels;
    uint16_t *x_map, *y_map;
    unsigned src_w, src_h;
    unsigned dst_w, dst_h;
    bool ok;
} jpeg_output_t;

static size_t jpeg_read(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_output_t *out = jd->device;
    jpeg_input_t *in = out->input;
    size_t left = in->len - in->pos;
    if (len > left) len = left;
    if (buf && len) memcpy(buf, in->data + in->pos, len);
    in->pos += len;
    return len;
}

static int jpeg_write(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_output_t *out = jd->device;
    const uint8_t *rgb = bitmap;
    unsigned rect_w = (unsigned)rect->right - rect->left + 1;
    unsigned dx0 = (unsigned)(((uint64_t)rect->left * out->dst_w) / out->src_w);
    unsigned dy0 = (unsigned)(((uint64_t)rect->top * out->dst_h) / out->src_h);
    unsigned dx1 = (unsigned)(((uint64_t)(rect->right + 1) * out->dst_w + out->src_w - 1) / out->src_w);
    unsigned dy1 = (unsigned)(((uint64_t)(rect->bottom + 1) * out->dst_h + out->src_h - 1) / out->src_h);
    if (dx1 > out->dst_w) dx1 = out->dst_w;
    if (dy1 > out->dst_h) dy1 = out->dst_h;

    for (unsigned dy = dy0; dy < dy1; dy++) {
        unsigned sy = out->y_map[dy];
        if (sy < rect->top || sy > rect->bottom) continue;
        for (unsigned dx = dx0; dx < dx1; dx++) {
            unsigned sx = out->x_map[dx];
            if (sx < rect->left || sx > rect->right) continue;
            const uint8_t *px = rgb + ((size_t)(sy - rect->top) * rect_w + (sx - rect->left)) * 3;
            out->pixels[(size_t)dy * out->dst_w + dx] =
                (uint16_t)(((px[2] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[0] >> 3));
        }
    }
    return out->ok;
}
#endif

static uint16_t *transform_pixels(const uint16_t *base, unsigned base_w, unsigned base_h,
                                  unsigned final_w, unsigned final_h,
                                  bool flip_h, bool flip_v, int rotation)
{
    uint16_t *out = malloc((size_t)final_w * final_h * sizeof(*out));
    if (!out) return NULL;
    for (unsigned y = 0; y < final_h; y++) {
        for (unsigned x = 0; x < final_w; x++) {
            unsigned bx, by;
            switch (rotation) {
            case 90:  bx = y; by = base_h - 1 - x; break;
            case 180: bx = base_w - 1 - x; by = base_h - 1 - y; break;
            case 270: bx = base_w - 1 - y; by = x; break;
            default:  bx = x; by = y; break;
            }
            if (flip_h) bx = base_w - 1 - bx;
            if (flip_v) by = base_h - 1 - by;
            out[(size_t)y * final_w + x] = base[(size_t)by * base_w + bx];
        }
    }
    return out;
}

static bool finish_frame(uint16_t *base, unsigned base_w, unsigned base_h,
                         unsigned final_w, unsigned final_h,
                         bool flip_h, bool flip_v, int rotation,
                         camera_frame_t **out)
{
    uint16_t *pixels = base;
    if (flip_h || flip_v || rotation) {
        pixels = transform_pixels(base, base_w, base_h, final_w, final_h,
                                  flip_h, flip_v, rotation);
        free(base);
        if (!pixels) return false;
    }
    camera_frame_t *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        free(pixels);
        return false;
    }
    frame->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    frame->dsc.header.w = (int32_t)final_w;
    frame->dsc.header.h = (int32_t)final_h;
    frame->dsc.header.stride = final_w * sizeof(*pixels);
    frame->dsc.data_size = (uint32_t)((size_t)final_w * final_h * sizeof(*pixels));
    frame->dsc.data = (const uint8_t *)pixels;
    *out = frame;
    return true;
}
#endif

bool camera_jpeg_decode(const uint8_t *jpeg, size_t jpeg_len, int max_w, int max_h,
                        bool flip_horizontal, bool flip_vertical, int rotation,
                        camera_frame_t **out)
{
#if LV_USE_TJPGD
    if (!jpeg || jpeg_len < 4 || max_w <= 0 || max_h <= 0 || !out) return false;
    *out = NULL;

#ifdef ESP_PLATFORM
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)jpeg_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
    unsigned src_w = info.width, src_h = info.height;
#else
    jpeg_input_t in = { jpeg, jpeg_len, 0 };
    jpeg_output_t decoded = { .input = &in, .ok = true };
    JDEC jd;
    uint8_t work[4096];
    if (camera_jd_prepare(&jd, jpeg_read, work, sizeof(work), &decoded) != JDR_OK) return false;
    unsigned src_w = jd.width, src_h = jd.height;
#endif
    if (!src_w || !src_h || src_w * src_h > 16777216UL) return false;

    bool quarter_turn = rotation == 90 || rotation == 270;
    unsigned oriented_w = quarter_turn ? src_h : src_w;
    unsigned oriented_h = quarter_turn ? src_w : src_h;
    unsigned final_w = oriented_w, final_h = oriented_h;
    if (final_w > (unsigned)max_w) {
        final_h = (unsigned)(((uint64_t)final_h * max_w) / final_w);
        final_w = (unsigned)max_w;
    }
    if (final_h > (unsigned)max_h) {
        final_w = (unsigned)(((uint64_t)final_w * max_h) / final_h);
        final_h = (unsigned)max_h;
    }
    if (!final_w || !final_h) return false;

    uint8_t scale = 0;
    unsigned dec_w = src_w, dec_h = src_h;
    for (uint8_t candidate = 1; candidate <= 3; candidate++) {
        unsigned cw = src_w >> candidate, ch = src_h >> candidate;
        if (!cw || !ch) break;
        unsigned ow = quarter_turn ? ch : cw, oh = quarter_turn ? cw : ch;
#ifdef ESP_PLATFORM
        if (ow * 3 < final_w || oh * 3 < final_h) break;
#else
        if (ow < final_w || oh < final_h) break;
#endif
        dec_w = cw;
        dec_h = ch;
        scale = candidate;
    }

    unsigned base_w = quarter_turn ? final_h : final_w;
    unsigned base_h = quarter_turn ? final_w : final_h;
    uint16_t *base = malloc((size_t)base_w * base_h * sizeof(*base));
    if (!base) return false;
    uint16_t *maps = malloc((size_t)(base_w + base_h) * sizeof(*maps));
    if (!maps) {
        free(base);
        return false;
    }
    for (unsigned x = 0; x < base_w; x++)
        maps[x] = (uint16_t)(((uint64_t)x * dec_w) / base_w);
    for (unsigned y = 0; y < base_h; y++)
        maps[base_w + y] = (uint16_t)(((uint64_t)y * dec_h) / base_h);

#ifdef ESP_PLATFORM
    cfg.out_scale = (esp_jpeg_image_scale_t)scale;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || !info.output_len) {
        free(maps);
        free(base);
        return false;
    }
    uint16_t *decoded = malloc(info.output_len);
    if (!decoded) {
        free(maps);
        free(base);
        return false;
    }
    cfg.outbuf = (uint8_t *)decoded;
    cfg.outbuf_size = (uint32_t)info.output_len;
    vTaskDelay(1);
    esp_err_t err = esp_jpeg_decode(&cfg, &info);
    vTaskDelay(1);
    if (err != ESP_OK || info.width != dec_w || info.height != dec_h) {
        free(decoded);
        free(maps);
        free(base);
        return false;
    }
    for (unsigned y = 0; y < base_h; y++) {
        for (unsigned x = 0; x < base_w; x++)
            base[(size_t)y * base_w + x] = decoded[(size_t)maps[base_w + y] * dec_w + maps[x]];
    }
    free(decoded);
#else
    decoded.pixels = base;
    decoded.x_map = maps;
    decoded.y_map = maps + base_w;
    decoded.src_w = dec_w;
    decoded.src_h = dec_h;
    decoded.dst_w = base_w;
    decoded.dst_h = base_h;
    JRESULT result = camera_jd_decomp(&jd, jpeg_write, scale);
    if (result != JDR_OK) {
        free(maps);
        free(base);
        return false;
    }
#endif
    free(maps);
    return finish_frame(base, base_w, base_h, final_w, final_h,
                        flip_horizontal, flip_vertical, rotation, out);
#else
    (void)jpeg; (void)jpeg_len; (void)max_w; (void)max_h;
    (void)flip_horizontal; (void)flip_vertical; (void)rotation; (void)out;
    return false;
#endif
}

void camera_frame_free(camera_frame_t *frame)
{
    if (!frame) return;
    free((void *)frame->dsc.data);
    free(frame);
}
