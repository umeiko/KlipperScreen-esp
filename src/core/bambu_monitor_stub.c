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
    bambu_status_reset(&out->printer);
}
