#pragma once

#include "camera.h"
#include "camera_jpeg.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct camera_stream camera_stream_t;

typedef enum {
    CAMERA_STREAM_CONNECTING = 0,
    CAMERA_STREAM_PLAYING,
    CAMERA_STREAM_RECONNECTING,
    CAMERA_STREAM_FAILED,
} camera_stream_state_t;

typedef struct {
    camera_stream_state_t state;
    uint32_t received_frames;
    uint32_t decode_errors;
    unsigned fps_x10;
} camera_stream_status_t;

camera_stream_t *camera_stream_start(const camera_info_t *camera, int max_w, int max_h);
void camera_stream_stop(camera_stream_t *stream);
bool camera_stream_poll(camera_stream_t *stream, camera_frame_t **frame,
                        camera_stream_status_t *status);

#ifdef __cplusplus
}
#endif
