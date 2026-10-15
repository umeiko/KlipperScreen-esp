
#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_BED_TURN_CW_SM
#define LV_ATTRIBUTE_IMG_BED_TURN_CW_SM
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_BED_TURN_CW_SM
uint8_t img_bed_turn_cw_sm_map[] = {

    0x00,0x01,0x2b,0x24,0x1b,
    0x03,0x5c,0x54,0x9d,0x2f,
    0x36,0x38,0x13,0x2d,0x29,
    0x24,0x5a,0x0a,0x4b,0x2f,
    0x00,0x1f,0x58,0x24,0x00,

};

const lv_image_dsc_t img_bed_turn_cw_sm = {
  .header = {
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_A8,
    .flags = 0,
    .w = 5,
    .h = 5,
    .stride = 5,
    .reserved_2 = 0,
  },
  .data_size = sizeof(img_bed_turn_cw_sm_map),
  .data = img_bed_turn_cw_sm_map,
  .reserved = NULL,
};
