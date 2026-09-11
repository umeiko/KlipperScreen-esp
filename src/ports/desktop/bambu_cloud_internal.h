#pragma once

#include "bambu_cloud.h"
#include <stdbool.h>
#include <stddef.h>

/* Internal bridge used only by the desktop MQTT transport.  Callers must wipe
 * the copied token after use and must never log it. */
bool bambu_cloud_copy_mqtt_credentials(char *user_id, size_t user_id_cap,
                                       char *token, size_t token_cap,
                                       bambu_cloud_region_t *region);
bool bambu_cloud_resolve_mqtt_user_id(const char *token,
                                      bambu_cloud_region_t region,
                                      char *user_id, size_t user_id_cap);
