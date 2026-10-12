#ifndef LV_CONF_H
#define LV_CONF_H
/* desktop 后端 LVGL v9 配置（未定义的项走 lv_conf_internal.h 默认值） */

#define LV_COLOR_DEPTH 16

#define LV_USE_OS LV_OS_NONE

/* 桌面端直接用系统 malloc（内置池默认仅 64KB，放不下 320x240 快照缓冲） */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB

/* SDL2 后端 */
#define LV_USE_SDL 1
#define LV_SDL_MOUSEWHEEL_MODE 0   /* encoder: wheel turns, middle button presses */
/* 必须 PARTIAL：SDL 驱动只在 PARTIAL 路径的 flush 里做 lv_draw_sw_rotate，
   默认 DIRECT 模式下 lv_display_set_rotation(90/270) 不转像素 → 花屏 */
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL

/* 字号 */
#define LV_FONT_MONTSERRAT_12 1   /* 小屏（160x128）图标/大数字档 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1   /* 大屏（800x480）图标档 */
#define LV_FONT_MONTSERRAT_48 1   /* 大屏大号数字档 */

/* 截图 */
#define LV_USE_SNAPSHOT 1

/* 压缩字体（font_cjk 全表 GB2312 6840 字走 RLE，否则 flash 放不下） */
#define LV_USE_FONT_COMPRESSED 1
/* 大字体支持（28/32 全表 glyph 索引超 20bit） */
#define LV_FONT_FMT_TXT_LARGE 1

/* PNG 解码（gcode 缩略图；lodepng 由 lv_init 自动注册） */
#define LV_USE_LODEPNG 1

#endif /* LV_CONF_H */
