#pragma once

/*
 * Bambu Cloud account sign-in.  The UI consumes only this small asynchronous
 * interface; TLS, token storage and platform credentials stay in the port.
 */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BAMBU_CLOUD_ACCOUNT_MAX 128
#define BAMBU_CLOUD_DEVICE_MAX  12

typedef enum {
    BAMBU_CLOUD_UNSUPPORTED = 0,
    BAMBU_CLOUD_SIGNED_OUT,
    BAMBU_CLOUD_BUSY,
    BAMBU_CLOUD_NEED_CODE,
    BAMBU_CLOUD_NEED_TFA,
    BAMBU_CLOUD_SIGNED_IN,
    BAMBU_CLOUD_FAILED,
} bambu_cloud_state_t;

typedef enum {
    BAMBU_CLOUD_REGION_GLOBAL = 0,
    BAMBU_CLOUD_REGION_CHINA,
} bambu_cloud_region_t;

typedef struct {
    char serial[40];
    char name[64];
    char model[48];
    bool online;
} bambu_cloud_device_t;

typedef struct {
    bambu_cloud_state_t state;
    bambu_cloud_region_t region;
    char account[BAMBU_CLOUD_ACCOUNT_MAX];
    char message[192];
    int device_count;
    bambu_cloud_device_t devices[BAMBU_CLOUD_DEVICE_MAX];
} bambu_cloud_snapshot_t;

void bambu_cloud_init(void);
void bambu_cloud_snapshot(bambu_cloud_snapshot_t *out);

/* Region can be changed only while signed out. */
bool bambu_cloud_set_region(bambu_cloud_region_t region);

/* Calls are non-blocking.  true means a worker accepted the request. */
bool bambu_cloud_login_password(const char *account, const char *password);
bool bambu_cloud_request_email_code(const char *email);
bool bambu_cloud_request_sms_code(const char *phone);
bool bambu_cloud_submit_code(const char *code);
bool bambu_cloud_refresh_devices(void);
void bambu_cloud_logout(void);

#ifdef __cplusplus
}
#endif
