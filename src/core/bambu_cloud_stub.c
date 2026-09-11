#include "bambu_cloud.h"
#include <string.h>

void bambu_cloud_init(void) {}

void bambu_cloud_snapshot(bambu_cloud_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = BAMBU_CLOUD_UNSUPPORTED;
    strcpy(out->message, "账号登录目前只在 Windows 桌面版提供");
}

bool bambu_cloud_set_region(bambu_cloud_region_t region)
{
    (void)region;
    return false;
}

bool bambu_cloud_login_password(const char *account, const char *password)
{
    (void)account; (void)password;
    return false;
}

bool bambu_cloud_request_email_code(const char *email)
{
    (void)email;
    return false;
}

bool bambu_cloud_request_sms_code(const char *phone)
{
    (void)phone;
    return false;
}

bool bambu_cloud_submit_code(const char *code)
{
    (void)code;
    return false;
}

bool bambu_cloud_refresh_devices(void) { return false; }
void bambu_cloud_logout(void) {}
