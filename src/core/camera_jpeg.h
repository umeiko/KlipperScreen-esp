#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_image_dsc_t dsc;
    uint32_t sequence;
} camera_frame_t;

bool camera_jpeg_decode(const uint8_t *jpeg, size_t jpeg_len, int max_w, int max_h,
                        bool flip_horizontal, bool flip_vertical, int rotation,
                        camera_frame_t **out);
void camera_frame_free(camera_frame_t *frame);

#ifdef __cplusplus
}
#endif
