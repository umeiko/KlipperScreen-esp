#pragma once
#include "bed_level.h"
#if defined(ESP_PLATFORM) && BSP_HAS_BED_LEVEL
bool moonraker_bed_level_start(void);
bool moonraker_bed_level_poll(bed_level_data_t *out);
void moonraker_bed_level_cancel(void);
#endif
