/*
 * 当前板型的构建名（与 tools/build-esp32.sh 的板型代号一致），About 页展示用。
 * 集中在 BSP 层按 Kconfig 板型宏判定，上层不直接感知 CONFIG_BOARD_*。
 */
#include "bsp.h"
#include "sdkconfig.h"

const char *bsp_board_name(void)
{
#if defined(CONFIG_BOARD_CYD_2432S028R)
    return "cyd_2432s028r";
#elif defined(CONFIG_BOARD_CYD_2432S028R_PLUS)
    return "cyd_2432s028r_plus";
#elif defined(CONFIG_BOARD_E32R35T)
    return "e32r35t";
#elif defined(CONFIG_BOARD_JC8048W550)
    return "jc8048w550";
#elif defined(CONFIG_BOARD_SENSECAP_INDICATOR)
    return "esp32s3-sensecap-indicator";
#elif defined(CONFIG_BOARD_ESP32S3_JLC_SZP)
    return "esp32s3-JLC-SZP";
#elif defined(CONFIG_BOARD_EC11_KNOB_MINIMAL)
    return "esp32s3-st7789-320_240-ec11";
#elif defined(CONFIG_BOARD_EC11_KNOB_ESP32)
    return "esp32-st7735s-128_160-ec11";
#elif defined(CONFIG_BOARD_EC11_KNOB_ESP32_ST7789)
    return "esp32-st7789-320_240-ec11";
#elif defined(CONFIG_BOARD_ESP32_ILI9341_EC11)
    return "esp32-ILI9341-320_240-ec11";
#elif defined(CONFIG_BOARD_ESP32_ST7796_EC11)
    return "esp32-ST7796-320_240-ec11";
#elif defined(CONFIG_BOARD_ESP32S3_ST7796_EC11)
    return "esp32s3-st7796-480_320-xpt2046-ec11";
#elif defined(CONFIG_BOARD_ESP32S3_ILI9488_EC11)
    return "esp32s3-ILI9488-480_320-xpt2046-ec11";
#elif defined(CONFIG_BOARD_ESP32S3_ILI9341_EC11)
    return "esp32s3-ILI9341-320_240-xpt2046-ec11";
#elif defined(CONFIG_BOARD_ESP32S3_RETRO_GO)
    return "esp32s3-retro-go";
#elif defined(CONFIG_BOARD_ESP32C3_ST7789_EC11)
    return "esp32c3-st7789-320_240-ec11";
#else
    return "unknown";
#endif
}
