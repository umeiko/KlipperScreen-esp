#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} camera_mjpeg_parser_t;

bool camera_mjpeg_init(camera_mjpeg_parser_t *parser, size_t cap);
void camera_mjpeg_free(camera_mjpeg_parser_t *parser);
bool camera_mjpeg_feed(camera_mjpeg_parser_t *parser, const uint8_t *data, size_t len,
                       uint8_t **jpeg, size_t *jpeg_len);

#ifdef __cplusplus
}
#endif
