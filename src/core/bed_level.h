#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bsp_caps.h"
#if BSP_HAS_BED_LEVEL
#define BED_LEVEL_POINTS 4
typedef enum { BED_LEVEL_LOADING, BED_LEVEL_MISSING, BED_LEVEL_INVALID,
               BED_LEVEL_FAILED, BED_LEVEL_READY } bed_level_state_t;
typedef struct {
    float x, y;
    char name[48], result[24];
} bed_level_point_t;
typedef struct {
    bed_level_state_t state;
    int count;
    bool probe, has_manual, busy, can_move, operation_failed, status_failed;
    float travel_z, paper_z, speed, lift_speed;
    bed_level_point_t points[BED_LEVEL_POINTS];
} bed_level_data_t;
/* Called only on the LVGL thread. Replies never hold pointers to panel objects. */
const bed_level_data_t *bed_level_poll(void);
void bed_level_reset(void);
bool bed_level_move(int point);
bool bed_level_calculate(void);
bool bed_level_motors_off(void);
#endif
