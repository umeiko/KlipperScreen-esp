#pragma once

/*
 * ESP32 Bambu Cloud runtime: the single bambu_net actor, cloud snapshot and
 * sign-in lifecycle.  UI code must use src/core/bambu_cloud.h instead; these
 * entry points back the thin public-API wrapper in bambu_cloud_esp32.c.
 *
 * All HTTPS runs on the actor task.  The public API only enqueues commands
 * and reads the mutex-protected snapshot; no network, JSON or NVS I/O ever
 * happens while the snapshot mutex is held.
 */
#include "bambu_cloud.h"
#include <stddef.h>

void bambu_rt_init(void);
void bambu_rt_snapshot(bambu_cloud_snapshot_t *out);

bool bambu_rt_set_region(bambu_cloud_region_t region);
bool bambu_rt_login_password(const char *account, const char *password);
bool bambu_rt_request_email_code(const char *email);
bool bambu_rt_request_sms_code(const char *phone);
bool bambu_rt_submit_code(const char *code);
bool bambu_rt_refresh_devices(void);
void bambu_rt_logout(void);

/* Internal bridge used by the ESP monitor.  Copies are bounded and remain
 * owned by the caller; callers must wipe them after destroying MQTT. */
bool bambu_rt_copy_mqtt_credentials(bambu_cloud_region_t *region,
                                    char *user_id, size_t user_id_cap,
                                    char *token, size_t token_cap);
void bambu_rt_wake(void);
void bambu_rt_wipe(void *ptr, size_t len);
