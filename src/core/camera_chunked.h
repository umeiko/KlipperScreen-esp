#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAMERA_CHUNK_SIZE = 0,
    CAMERA_CHUNK_DATA,
    CAMERA_CHUNK_DATA_CR,
    CAMERA_CHUNK_DATA_LF,
    CAMERA_CHUNK_TRAILER,
    CAMERA_CHUNK_DONE,
    CAMERA_CHUNK_ERROR,
} camera_chunked_state_t;

typedef struct {
    camera_chunked_state_t state;
    size_t remaining;
    char line[128];
    size_t line_len;
} camera_chunked_parser_t;

void camera_chunked_init(camera_chunked_parser_t *parser);
bool camera_chunked_feed(camera_chunked_parser_t *parser,
                         const uint8_t *data, size_t len,
                         uint8_t *out, size_t out_cap, size_t *out_len,
                         bool *done);

#ifdef __cplusplus
}
#endif
