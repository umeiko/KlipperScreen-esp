#include "bambu_status.h"
#include "cJSON.h"

#include <string.h>

static cJSON *field(cJSON *obj, const char *name)
{
    return cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, name) : NULL;
}

static bool merge_number(cJSON *obj, const char *name, float *value)
{
    cJSON *v = field(obj, name);
    if (!cJSON_IsNumber(v)) return false;
    *value = (float)v->valuedouble;
    return true;
}

static bool merge_int(cJSON *obj, const char *name, int *value)
{
    cJSON *v = field(obj, name);
    if (!cJSON_IsNumber(v)) return false;
    *value = (int)v->valuedouble;
    return true;
}

static bambu_print_state_t parse_state(const char *value)
{
    if (!value) return BAMBU_PRINT_UNKNOWN;
    if (strcmp(value, "RUNNING") == 0) return BAMBU_PRINT_RUNNING;
    if (strcmp(value, "PAUSE") == 0) return BAMBU_PRINT_PAUSED;
    if (strcmp(value, "PREPARE") == 0) return BAMBU_PRINT_PREPARE;
    if (strcmp(value, "FINISH") == 0) return BAMBU_PRINT_FINISHED;
    if (strcmp(value, "FAILED") == 0) return BAMBU_PRINT_FAILED;
    if (strcmp(value, "IDLE") == 0) return BAMBU_PRINT_IDLE;
    return BAMBU_PRINT_UNKNOWN;
}

void bambu_status_reset(bambu_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->state = BAMBU_PRINT_UNKNOWN;
    status->remaining_minutes = -1;
}

bool bambu_status_apply_json(bambu_status_t *status,
                             const char *json, size_t json_len)
{
    if (!status || !json || json_len == 0) return false;
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) return false;
    cJSON *print = field(root, "print");
    if (!cJSON_IsObject(print)) {
        cJSON_Delete(root);
        return false;
    }

    bool changed = false;
    cJSON *v = field(print, "gcode_state");
    if (cJSON_IsString(v) && v->valuestring) {
        bambu_print_state_t next = parse_state(v->valuestring);
        if (next != BAMBU_PRINT_UNKNOWN) status->state = next;
        changed = true;
    }
    changed |= merge_number(print, "nozzle_temper", &status->nozzle_temp);
    changed |= merge_number(print, "nozzle_target_temper", &status->nozzle_target);
    changed |= merge_number(print, "bed_temper", &status->bed_temp);
    changed |= merge_number(print, "bed_target_temper", &status->bed_target);
    changed |= merge_int(print, "mc_percent", &status->progress_percent);
    changed |= merge_int(print, "mc_remaining_time", &status->remaining_minutes);
    changed |= merge_int(print, "layer_num", &status->layer_current);
    changed |= merge_int(print, "total_layer_num", &status->layer_total);

    v = field(print, "subtask_name");
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(status->task_name, v->valuestring, sizeof(status->task_name) - 1);
        status->task_name[sizeof(status->task_name) - 1] = 0;
        changed = true;
    } else {
        v = field(print, "gcode_file");
        if (cJSON_IsString(v) && v->valuestring) {
            strncpy(status->task_name, v->valuestring, sizeof(status->task_name) - 1);
            status->task_name[sizeof(status->task_name) - 1] = 0;
            changed = true;
        }
    }

    if (status->progress_percent < 0) status->progress_percent = 0;
    if (status->progress_percent > 100) status->progress_percent = 100;
    if (changed) status->has_data = true;
    cJSON_Delete(root);
    return changed;
}
