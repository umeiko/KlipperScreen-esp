#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 充电屏保：电压>5V 且空闲超时后播放供奉动图，任意输入退出。 */
void screensaver_init(void);
void screensaver_poll(void);
int  screensaver_is_active(void);

#ifdef __cplusplus
}
#endif
