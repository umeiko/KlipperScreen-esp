#pragma once

/*
 * Bambu MQTT reports are incremental patches: a missing field means
 * "unchanged", not zero.  This transport-neutral state object keeps the merge
 * rule in one place so the Windows/OpenSSL client and a future ESP-IDF MQTT
 * port can share the same printer semantics.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAMBU_PRINT_UNKNOWN = 0,
    BAMBU_PRINT_IDLE,
    BAMBU_PRINT_RUNNING,
    BAMBU_PRINT_PAUSED,
    BAMBU_PRINT_PREPARE,
    BAMBU_PRINT_FINISHED,
    BAMBU_PRINT_FAILED,
} bambu_print_state_t;

typedef struct {
    bool has_data;
    bambu_print_state_t state;
    float nozzle_temp;
    float nozzle_target;
    float bed_temp;
    float bed_target;
    int progress_percent;
    int remaining_minutes;
    int layer_current;
    int layer_total;
    char task_name[96];
} bambu_status_t;

void bambu_status_reset(bambu_status_t *status);
bool bambu_status_apply_json(bambu_status_t *status,
                             const char *json, size_t json_len);

#ifdef __cplusplus
}
#endif
