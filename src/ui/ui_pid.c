#include "ui_pid.h"

#define UI_PID_KP 140.0f
#define UI_PID_KI 5.0f
#define UI_PID_KD 22.0f
#define UI_PID_DT_MAX 0.05f
#define UI_PID_INTEGRAL_MAX 50.0f
#define UI_PID_ACCEL_MAX 12000.0f
#define UI_PID_VELOCITY_MAX 2400.0f

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

    float error = target - pid->position;
    float integral = clampf(pid->integral + error * dt, UI_PID_INTEGRAL_MAX);
    float acceleration = UI_PID_KP * error + UI_PID_KI * integral - UI_PID_KD * pid->velocity;
    if ((acceleration > UI_PID_ACCEL_MAX && error > 0.0f) ||
        (acceleration < -UI_PID_ACCEL_MAX && error < 0.0f)) {
        acceleration = UI_PID_KP * error + UI_PID_KI * pid->integral - UI_PID_KD * pid->velocity;
    } else {
        pid->integral = integral;
    }

    acceleration = clampf(acceleration, UI_PID_ACCEL_MAX);
    pid->velocity = clampf(pid->velocity + acceleration * dt, UI_PID_VELOCITY_MAX);
    pid->position += pid->velocity * dt;
    return pid->position;
}
