#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_MAX_CAMERAS 8
#define CAMERA_NAME_CAP 64
#define CAMERA_URL_CAP 384

typedef struct {
    char name[CAMERA_NAME_CAP];
    char stream_url[CAMERA_URL_CAP];
    char snapshot_url[CAMERA_URL_CAP];
    bool enabled;
    bool flip_horizontal;
    bool flip_vertical;
    int rotation;
} camera_info_t;

typedef struct {
    camera_info_t items[CAMERA_MAX_CAMERAS];
    int count;
} camera_list_t;

bool camera_parse_list_json(const char *json, camera_list_t *out);
bool camera_build_url(const char *moonraker_host, uint16_t moonraker_port,
                      const char *raw_url, char *out, size_t out_cap);
bool camera_parse_http_url(const char *url, char *host, size_t host_cap,
                           uint16_t *port, char *path, size_t path_cap);

#ifdef __cplusplus
}
#endif
