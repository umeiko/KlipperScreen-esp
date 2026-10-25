#include "ui_pid.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void converge(ui_pid_t *pid, float target, float dt)
{
    for (unsigned i = 0; i < 3000; i++) ui_pid_step(pid, target, dt);
    assert(isfinite(pid->position));
    assert(isfinite(pid->velocity));
    assert(fabsf(pid->position - target) < 0.75f);
    assert(fabsf(pid->velocity) < 0.75f);
}

int main(void)
{
    ui_pid_t pid;
    ui_pid_reset(&pid, 10.0f);
    assert(pid.position == 10.0f && pid.integral == 0.0f && pid.velocity == 0.0f);
    assert(ui_pid_step(&pid, 100.0f, 0.0f) == 10.0f);

    ui_pid_reset(&pid, 0.0f);
    for (unsigned i = 0; i < 24; i++) ui_pid_step(&pid, 100.0f, 1.0f / 60.0f);
    assert(fabsf(pid.position - 100.0f) < 1.0f);

    converge(&pid, 130.0f, 1.0f / 60.0f);
    converge(&pid, -40.0f, 1.0f / 60.0f);
    converge(&pid, 1000.0f, 1.0f / 30.0f);

    ui_pid_reset(&pid, 0.0f);
    for (unsigned i = 0; i < 3000; i++) {
        const float steps[] = { 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 120.0f };
        ui_pid_step(&pid, 75.0f, steps[i % 3]);
    }
    assert(fabsf(pid.position - 75.0f) < 0.75f);
    assert(fabsf(pid.velocity) < 0.75f);

    ui_pid_reset(&pid, 20.0f);
    assert(pid.position == 20.0f && pid.integral == 0.0f && pid.velocity == 0.0f);
    puts("PASS: focus PID reset, retargeting, convergence and variable timestep");
    return 0;
}
