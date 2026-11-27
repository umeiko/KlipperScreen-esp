#pragma once
#include <stdbool.h>

/* ESP32 timezone presets. IDs are persisted; labels use standard UTC/DST names. */
unsigned app_timezone_count(void);
const char *app_timezone_label(unsigned index);
unsigned app_timezone_current(void);
bool app_timezone_apply(unsigned index);
bool app_timezone_select(unsigned index);
