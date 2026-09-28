#include "bsp.h"
#include "bsp_wifi.h"
#include "ui_app.h"
#include "boot_anim.h"
#include "app_settings.h"
#include "screensaver.h"
#include "web_config.h"

#if CONFIG_BOARD_ESP32S3_ST7789
#include "moonraker_client.h"
#include "confirm.h"
#include <stdio.h>

static int s_hotkey_slot = -1;

/* 弹窗点"确认"后真正切换 */
static void on_hotkey_confirm(void *ud)
{
    (void)ud;
    if (s_hotkey_slot >= 0) {
        settings_save_active_printer(s_hotkey_slot);
        moonraker_reload();   /* 立即重连新槽位 */
        s_hotkey_slot = -1;
    }
}

/* 快捷键按下：弹窗"切换到打印机 N？"，确认/取消 */
static void on_hotkey_select(int slot)
{
    s_hotkey_slot = slot;
    char msg[48];
    snprintf(msg, sizeof(msg), "切换到打印机 %d？", slot + 1);
    confirm_open(msg, "确认", on_hotkey_confirm, NULL);
}
#endif

void debug_cli_start(void);

void app_main(void)
{
    bsp_init();            /* 显示 + LVGL 任务（核 1） */

    bsp_lvgl_lock();
    bsp_input_init();      /* 可选附加输入（Kconfig 旋转编码器） */
    boot_anim_play(bsp_lcd_push, bsp_delay_ms);   /* 开机动画（~2.5s） */
    ui_app_create();       /* 与 desktop 后端共享的同一份 UI 代码 */
    screensaver_init();    /* 充电供奉屏保 */
    bsp_set_brightness(settings_load_brightness());   /* 背光偏好 */
    bsp_set_screen_timeout(settings_load_screen_off());   /* 自动息屏（0=永不） */
    bsp_lvgl_unlock();

    bsp_disp_set_invert(settings_load_display_invert());    /* 反色偏好 */
    bsp_disp_set_rotate180(settings_load_display_rotate()); /* 180° 旋转偏好 */

#if CONFIG_BOARD_ESP32S3_ST7789
    /* 注册快捷键 → 打印机切换回调（仅 ST7789 板型） */
    bsp_hotkey_set_select_handler(on_hotkey_select);
#endif

    /* WiFi 自动回连 */
    wifi_conf_t wc;
    if (settings_load_wifi(&wc))
        bsp_wifi_connect(wc.ssid, wc.pass[0] ? wc.pass : NULL);
    web_config_start();      /* 手机后台配置页 :80 */

    debug_cli_start();       /* 串口调试命令行 */
}
