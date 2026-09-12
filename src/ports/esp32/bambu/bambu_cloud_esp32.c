/*
 * Public Bambu Cloud API for ESP32: thin forwarding layer over the
 * bambu_net actor runtime.  All blocking work lives in
 * bambu_runtime_esp32.c; these functions only enqueue commands or copy the
 * mutex-protected snapshot.
 */
#include "bambu_cloud.h"
#include "bambu_runtime_esp32.h"

void bambu_cloud_init(void)
{
    bambu_rt_init();
}

void bambu_cloud_snapshot(bambu_cloud_snapshot_t *out)
{
    bambu_rt_snapshot(out);
}

bool bambu_cloud_set_region(bambu_cloud_region_t region)
{
    return bambu_rt_set_region(region);
}

bool bambu_cloud_login_password(const char *account, const char *password)
{
    return bambu_rt_login_password(account, password);
}

bool bambu_cloud_request_email_code(const char *email)
{
    return bambu_rt_request_email_code(email);
}

bool bambu_cloud_request_sms_code(const char *phone)
{
    return bambu_rt_request_sms_code(phone);
}

bool bambu_cloud_submit_code(const char *code)
{
    return bambu_rt_submit_code(code);
}

bool bambu_cloud_refresh_devices(void)
{
    return bambu_rt_refresh_devices();
}

void bambu_cloud_logout(void)
{
    bambu_rt_logout();
}
