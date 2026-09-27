/*
 * moonraker_client 空桩（desktop simulator）：不连真实上位机，数据走 printer_mock。
 */
#include "moonraker_client.h"

void moonraker_start(void) {}
void moonraker_stop(void) {}
void moonraker_reload(void) {}
moonraker_state_t moonraker_state(void) { return MOONRAKER_OFFLINE; }
bool moonraker_send_rpc(const char *method, const char *params_json)
{
    (void)method; (void)params_json;
    return false;
}
bool moonraker_rpc(const char *method, const char *params_json,
                   void (*cb)(char *result_json, void *ud), void *ud)
{
    (void)method; (void)params_json; (void)cb; (void)ud;
    return false;
}
