#include "ui_pid.h"
#include <math.h>

#define UI_PID_KP 280.0f
#define UI_PID_KI 60.0f
#define UI_PID_KD 20.0f
#define UI_PID_DT_MAX 0.05f
#define UI_PID_SUBSTEP (1.0f / 240.0f)
#define UI_PID_INTEGRAL_MAX 60.0f
#define UI_PID_ACCEL_MAX 30000.0f
#define UI_PID_VELOCITY_MAX 1800.0f

static float clampf(float value, float limit)
{
    if (value > limit) return limit;
    if (value < -limit) return -limit;
    return value;
}

void ui_pid_reset(ui_pid_t *pid, float position)
{
    pid->position = position;
    pid->integral = 0.0f;
    pid->velocity = 0.0f;
}

float ui_pid_step(ui_pid_t *pid, float target, float dt)
{
    if (dt <= 0.0f) return pid->position;
    if (dt > UI_PID_DT_MAX) dt = UI_PID_DT_MAX;

    unsigned steps = (unsigned)(dt / UI_PID_SUBSTEP) + 1;
    float step_dt = dt / (float)steps;
    float decay = expf(-3.0f * step_dt);
    for (unsigned i = 0; i < steps; i++) {
        float error = target - pid->position;
        float integral = clampf(pid->integral * decay + error * step_dt,
                                UI_PID_INTEGRAL_MAX);
        float acceleration = UI_PID_KP * error + UI_PID_KI * integral - UI_PID_KD * pid->velocity;
        if ((acceleration > UI_PID_ACCEL_MAX && error > 0.0f) ||
            (acceleration < -UI_PID_ACCEL_MAX && error < 0.0f)) {
            acceleration = UI_PID_KP * error + UI_PID_KI * pid->integral - UI_PID_KD * pid->velocity;
        } else {
            pid->integral = integral;
        }

        acceleration = clampf(acceleration, UI_PID_ACCEL_MAX);
        pid->velocity = clampf(pid->velocity + acceleration * step_dt, UI_PID_VELOCITY_MAX);
        pid->position += pid->velocity * step_dt;
    }
    return pid->position;
}
