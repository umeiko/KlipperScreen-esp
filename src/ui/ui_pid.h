#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float position;
    float integral;
    float velocity;
} ui_pid_t;

void ui_pid_reset(ui_pid_t *pid, float position);
float ui_pid_step(ui_pid_t *pid, float target, float dt);

#ifdef __cplusplus
}
#endif
