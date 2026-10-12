#pragma once

#include <stdbool.h>

/*
 * bambu_net actor hooks.  Public monitor calls only update desired state and
 * wake the actor; all MQTT create/start/stop/destroy operations happen from
 * that actor so HTTPS and MQTT TLS sessions can never overlap on CYD.
 */
void bambu_monitor_actor_tick(void);
bool bambu_monitor_actor_needs_tick(void);
void bambu_monitor_actor_suspend(void);
void bambu_monitor_actor_resume(void);
