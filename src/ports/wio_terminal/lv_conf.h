#ifndef LV_CONF_H
#define LV_CONF_H

/* Seeed Wio Terminal: 320x240 RGB565, 192KB SRAM, no RTOS. */
#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD 20

/* Wio provides compatible no-kerning copies of these four faces.  LVGL's
   stock files carry ~3 KiB of kerning classes per size, an expensive detail
   at 12-20px on a 496 KiB target. */
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(lv_font_montserrat_12); \
    LV_FONT_DECLARE(lv_font_montserrat_14); \
    LV_FONT_DECLARE(lv_font_montserrat_16); \
    LV_FONT_DECLARE(lv_font_montserrat_20)
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FONT_COMPRESSED 1
#define LV_USE_LODEPNG 0
#define LV_USE_SNAPSHOT 0

/* The shared UI only needs the widgets left at their LVGL defaults below.
   Turning off the unused catalogue matters on a 496 KiB MCU even with
   --gc-sections because several class descriptors reference their handlers. */
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

#endif /* LV_CONF_H */
