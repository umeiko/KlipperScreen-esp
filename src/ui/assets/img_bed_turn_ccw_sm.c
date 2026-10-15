
#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_BED_TURN_CCW_SM
#define LV_ATTRIBUTE_IMG_BED_TURN_CCW_SM
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_BED_TURN_CCW_SM
uint8_t img_bed_turn_ccw_sm_map[] = {

    0x1c,0x26,0x2b,0x01,0x00,
    0x30,0x9e,0x52,0x5b,0x03,
    0x2b,0x2d,0x13,0x3b,0x35,
    0x31,0x4a,0x0a,0x5b,0x23,
    0x00,0x26,0x58,0x1e,0x00,

};

const lv_image_dsc_t img_bed_turn_ccw_sm = {
  .header = {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_A8,
    .flags = 0,
    .w = 5,
    .h = 5,
    .stride = 5,
    .reserved_2 = 0,
  },
  .data_size = sizeof(img_bed_turn_ccw_sm_map),
  .data = img_bed_turn_ccw_sm_map,
  .reserved = NULL,
};
