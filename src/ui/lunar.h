#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int year, month, day, leap; } lunar_t;
/* 公历(y,m,d) -> 农历。范围 1900-01-31 .. 2100-12-29。leap=1 表示闰月。 */
void solar_to_lunar(int sy, int sm, int sd, lunar_t *out);
#ifdef __cplusplus
}
#endif
