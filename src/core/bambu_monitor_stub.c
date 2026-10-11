#include "bambu_monitor.h"
#include <string.h>

void bambu_monitor_init(void) {}
void bambu_monitor_start(const char *serial) { (void)serial; }
void bambu_monitor_stop(void) {}
void bambu_monitor_snapshot(bambu_monitor_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = BAMBU_MONITOR_STOPPED;
    /* Keep the unsupported stub independent from the JSON parser/cJSON. */
    out->printer.state = BAMBU_PRINT_UNKNOWN;
    out->printer.remaining_minutes = -1;
}
