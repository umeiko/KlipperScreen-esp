#pragma once

#include "bambu_status.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BAMBU_MONITOR_STOPPED = 0,
    BAMBU_MONITOR_CONNECTING,
    BAMBU_MONITOR_CONNECTED,
    BAMBU_MONITOR_AUTH_ERROR,
    BAMBU_MONITOR_NETWORK_ERROR,
} bambu_monitor_state_t;

typedef struct {
    bambu_monitor_state_t state;
    bool connected;
    char message[128];
    bambu_status_t printer;
} bambu_monitor_snapshot_t;

void bambu_monitor_init(void);
void bambu_monitor_start(const char *serial);
void bambu_monitor_stop(void);
void bambu_monitor_snapshot(bambu_monitor_snapshot_t *out);

#ifdef __cplusplus
}
#endif
