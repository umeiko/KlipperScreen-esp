#pragma once
#include "bed_level.h"
#if BSP_HAS_BED_LEVEL
#include <stddef.h>
/* Retain selected config sections only, not macro bodies / the complete DOM. */
typedef struct {
    char json[6144], key[64];
    unsigned char role[24], container[24], expect[24];
    unsigned depth, capture_depth, key_len;
    size_t used, bytes;
    bool string, escape, is_key, key_overflow, capture, found, failed, closed;
} bed_level_config_stream_t;
void bed_level_config_begin(bed_level_config_stream_t *s);
bool bed_level_config_feed(bed_level_config_stream_t *s, const char *data, size_t size);
bool bed_level_config_finish(bed_level_config_stream_t *s, bed_level_data_t *out);
#endif
