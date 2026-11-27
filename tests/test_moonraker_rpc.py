"""Exercise the real ESP32 RPC sender with offline transport/allocation mocks."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void cJSON;
@PENDING_TYPE@
static bool disabled;
static void *ws = (void *)1;
static int next_id = 4;
static pending_t pending[8];
static int allocations, send_calls;
static bool fail_alloc, fail_send;
static char captured[8192];
#define TAG "test"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)

void *frame_malloc(size_t size) {
    if (fail_alloc) return NULL;
    void *p = malloc(size);
    if (p) allocations++;
    return p;
}
void frame_free(void *p) {
    if (p) allocations--;
    free(p);
}
static bool esp_websocket_client_is_connected(void *client) { return client != NULL; }
static int esp_websocket_client_send_text(void *client, const char *frame, int len, int timeout) {
    assert(client == ws && timeout == 2000);
    assert(len > 0 && (size_t)len < sizeof(captured));
    assert(frame[len] == '\0');
    memcpy(captured, frame, (size_t)len + 1);
    send_calls++;
    return fail_send ? -1 : len;
}
#define malloc frame_malloc
#define free frame_free
@SENDER@
#undef malloc
#undef free

static void reset(void) {
    assert(allocations == 0);
    memset(pending, 0, sizeof(pending));
    next_id = 4;
    send_calls = 0;
    fail_alloc = fail_send = false;
    captured[0] = '\0';
}
static void assert_no_pending(void) {
    for (size_t i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
        assert(pending[i].id == 0);
    assert(allocations == 0);
}
int main(void) {
    const char *params = @PARAMS@;
    const char *expected = @EXPECTED@;
    /* The U1 fixture must reach the actual sender intact, beyond the old 1KB limit. */
    assert(strlen(expected) == 1156);
    assert(send_rpc_full("printer.objects.subscribe", params, NULL, NULL, NULL));
    assert(send_calls == 1 && strcmp(captured, expected) == 0);
    assert(allocations == 0 && pending[0].id == 4);

    reset();
    assert(send_rpc_full("server.info", NULL, NULL, NULL, NULL));
    assert(strcmp(captured, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"server.info\"}") == 0);
    assert(allocations == 0);

    reset();
    fail_alloc = true;
    for (int i = 0; i < 12; i++) {
        assert(!send_rpc_full("printer.objects.subscribe", params, NULL, NULL, NULL));
        assert_no_pending();
    }
    assert(send_calls == 0);
    fail_alloc = false;
    assert(send_rpc_full("printer.objects.subscribe", params, NULL, NULL, NULL));
    assert(allocations == 0);  /* Allocation failures did not exhaust pending slots. */

    reset();
    fail_send = true;
    assert(!send_rpc_full("printer.objects.subscribe", params, NULL, NULL, NULL));
    assert(send_calls == 1);
    assert_no_pending();
    puts("ESP32 Moonraker RPC: U1 1156-byte frame, no params, allocation/send failure checks passed");
    return 0;
}
'''


def main():
    cc = os.environ.get("CC") or shutil.which("cc")
    if not cc:
        raise SystemExit("cc required (or set CC)")
    source = (REPO / "src/core/moonraker_client.c").read_text(encoding="utf-8")
    # Compile the production static functions; do not maintain a second sender implementation.
    pending_type = source[source.index("typedef struct {"):source.index("} pending_t;") + len("} pending_t;")]
    sender = source.split("/* ---------- 发送 ---------- */", 1)[1].split("static bool send_rpc_cb", 1)[0]
    objects = {
        "webhooks": None,
        "print_stats": ["state", "filename", "print_duration", "total_duration", "message"],
        "virtual_sdcard": ["progress", "is_active"],
        "display_status": ["progress", "message"],
        "gcode_move": ["speed_factor", "extrude_factor", "homing_origin"],
        "toolhead": ["position", "homed_axes"],
        "extruder": ["temperature", "target", "power"],
        "heater_bed": ["temperature", "target", "power"],
        "fan": ["speed"], "idle_timeout": ["state"],
        "manual_probe": ["is_active"], "pause_resume": ["is_paused"],
    }
    fans = ["fan_generic cavity_fan", "heater_fan power_fan"]
    fans += [f"heater_fan e{i}_nozzle_fan" for i in range(4)]
    fans += [f"fan_generic e{i}_fan" for i in range(1, 4)]
    objects.update((name, ["speed"]) for name in fans)
    objects.update((f"filament_motion_sensor e{i}_filament", ["enabled", "filament_detected"]) for i in range(4))
    objects["probe"] = ["z_offset"]
    params = json.dumps({"objects": objects}, separators=(",", ":"))
    expected = json.dumps({"jsonrpc": "2.0", "id": 4, "method": "printer.objects.subscribe",
                           "params": {"objects": objects}}, separators=(",", ":"))
    harness = HARNESS.replace("@PENDING_TYPE@", pending_type).replace("@SENDER@", sender)
    harness = harness.replace("@PARAMS@", json.dumps(params)).replace("@EXPECTED@", json.dumps(expected))
    with tempfile.TemporaryDirectory(prefix="kr-moonraker-rpc-") as directory:
        root = Path(directory)
        (root / "test.c").write_text(harness, encoding="utf-8")
        exe = root / ("test.exe" if os.name == "nt" else "test")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(root / "test.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
