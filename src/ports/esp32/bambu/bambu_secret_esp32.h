#pragma once

/*
 * ESP32 Bambu Cloud credential profile in NVS.
 *
 * One fixed-layout blob holds the cloud region, account, user id and access
 * token (schema-versioned).  Passwords, verification codes and TFA keys are
 * never stored.  NVS is NOT encrypted here: with flash encryption disabled
 * the token can be read out physically; logout erases it.
 */
#include <stdbool.h>
#include <stdint.h>

#define BAMBU_SECRET_SCHEMA     1u
#define BAMBU_SECRET_TOKEN_MAX  2048

typedef struct {
    uint32_t schema;                     /* BAMBU_SECRET_SCHEMA */
    uint32_t region;                     /* bambu_cloud_region_t */
    char account[128];
    char user_id[96];
    char token[BAMBU_SECRET_TOKEN_MAX];
} bambu_secret_t;

/* true when a valid signed-in profile was loaded.  Corrupt or absent blobs
 * simply count as signed out. */
bool bambu_secret_load(bambu_secret_t *out);

bool bambu_secret_save(uint32_t region, const char *account,
                       const char *user_id, const char *token);

void bambu_secret_clear(void);
