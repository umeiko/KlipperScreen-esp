#pragma once
#include <stdbool.h>
#include <time.h>

/* Parse an HTTP Date as UTC without changing the process-wide display TZ. */
bool bsp_http_date_parse(const char *date, time_t *epoch);
