/* 板型能力宏：板型知识只允许出现在 BSP 层。
 * 上层（ui / core / ports）需要按能力裁剪时，include 本头并判断能力宏，
 * 不得再直接判断 CONFIG_BOARD_*。新增板型时在下方登记其能力。 */
#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* 按输入能力而非板名裁剪，普通桌面端不展示硬件设置。 */
#if defined(ESP_PLATFORM) && defined(CONFIG_INPUT_ROTARY_ENCODER) && CONFIG_INPUT_ROTARY_ENCODER
#define BSP_HAS_ROTARY_ENCODER 1
#else
#define BSP_HAS_ROTARY_ENCODER 0
#endif

/* 开发模拟器只有显式开启预览时才展示同一套设置交互。 */
#if BSP_HAS_ROTARY_ENCODER || (defined(KLIPPER_DESKTOP_SIMULATOR) && defined(KR_ENCODER_SETTINGS_PREVIEW))
#define BSP_HAS_ENCODER_SETTINGS 1
#else
#define BSP_HAS_ENCODER_SETTINGS 0
#endif

/* BSP_HAS_TOUCH_CAL：板型带电阻触摸层（XPT2046），支持两点校准，
 * 开放 CLI `caltouch` 强制重校；电容屏与无触摸板型为 0
 * （无触摸纯旋钮板——st7735s/st7789/ILI9341 ec11 机型与
 *   CONFIG_BOARD_ESP32_ST7796_EC11（esp32-ST7796-320_240-ec11）——走 else = 0） */
#if defined(CONFIG_BOARD_CYD_2432S028R) || defined(CONFIG_BOARD_CYD_2432S028R_PLUS) || \
    defined(CONFIG_BOARD_E32R35T) || \
    defined(CONFIG_BOARD_ESP32S3_ST7796_EC11) || defined(CONFIG_BOARD_ESP32S3_ILI9488_EC11) || \
    defined(CONFIG_BOARD_ESP32S3_ILI9341_EC11)
#define BSP_HAS_TOUCH_CAL 1
#else
#define BSP_HAS_TOUCH_CAL 0
#endif

/* BSP_HAS_BUTTONS：板型注册了 GPIO 实体按钮导航后端（bsp_gpio_buttons，
   上/下/左/右/确定/返回喂 ui_buttons 语义层）；其余板型为 0，输入行为不变 */
#if defined(CONFIG_BOARD_ESP32S3_RETRO_GO)
#define BSP_HAS_BUTTONS 1
#else
#define BSP_HAS_BUTTONS 0
#endif

/* BSP_HAS_DISPLAY_ROTATION：桌面端（Windows/Linux/macOS）由 LVGL SDL 驱动
 * 软件旋转（lv_display_set_rotation，flush 时 lv_draw_sw_rotate + 触摸坐标
 * 由 LVGL 内核自动反变换），显示设置提供 0/90/180/270 四档；
 * ESP32 各板走硬件 rotate180（bsp_disp_set_rotate180），不提供本能力。 */
#if !defined(ESP_PLATFORM)
#define BSP_HAS_DISPLAY_ROTATION 1
#else
#define BSP_HAS_DISPLAY_ROTATION 0
#endif

/* BSP_HAS_LINUX_HOST：Linux 上位机（红米等手机/树莓派）：
 * 背光写 /sys/class/backlight、电源键读 /dev/input（evdev KEY_POWER）。
 * Windows/macOS 桌面与 ESP32 为 0。 */
#if defined(__linux__) && !defined(ESP_PLATFORM)
#define BSP_HAS_LINUX_HOST 1
#else
#define BSP_HAS_LINUX_HOST 0
#endif
