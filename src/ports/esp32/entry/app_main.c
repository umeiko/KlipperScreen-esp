#include "bsp.h"
#include "bsp_wifi.h"
#include "ui_app.h"
#include "boot_anim.h"
#include "app_settings.h"
#include "bsp_caps.h"

#if BSP_HAS_BUTTONS
#include "bsp_gpio_buttons.h"
#include "ui_buttons.h"

/* GPIO 实体按钮后端（板型键表在 BSP 层注册）→ ui_buttons 语义层；
   两边枚举按同一顺序定义，静态断言钉死数值对应关系。 */
_Static_assert((int)BSP_NAV_KEY_UP == (int)UI_BTN_UP &&
               (int)BSP_NAV_KEY_DOWN == (int)UI_BTN_DOWN &&
               (int)BSP_NAV_KEY_LEFT == (int)UI_BTN_LEFT &&
               (int)BSP_NAV_KEY_RIGHT == (int)UI_BTN_RIGHT &&
               (int)BSP_NAV_KEY_OK == (int)UI_BTN_OK &&
               (int)BSP_NAV_KEY_BACK == (int)UI_BTN_BACK &&
               (int)BSP_NAV_KEY_COUNT == (int)UI_BTN_COUNT,
               "nav key enum mismatch");

static void nav_button_forward(int key, bool pressed)
{
    ui_buttons_send((ui_button_id_t)key, pressed);
}
#endif

void debug_cli_start(void);

void app_main(void)
{
    bsp_init();            /* 显示 + 触摸 + LVGL 任务（核 1） */
    settings_seed_defaults();   /* 与 desktop 对称；ESP32 的 bsp 实现为空操作 */

    bsp_lvgl_lock();
    bsp_input_init();      /* 可选附加输入（Kconfig 旋转编码器），可与触摸并存 */
#if BSP_HAS_ROTARY_ENCODER
    bsp_encoder_set_counts_per_detent(settings_load_encoder_counts());
  #endif
    if (bsp_disp_can_color_order())
        bsp_disp_set_color_order(settings_load_display_color_order());
    /* 反色/旋转/镜像偏好必须在开机动画之前应用：动画经 bsp_lcd_push 走
       面板硬件镜像/反色寄存器，先配置动画才能跟随用户的显示方向偏好。
       同时也满足"面板命令须在 LVGL 首帧 flush 前完成"的约束（ui_app_create
       之前 LVGL 无任何画面可刷）。 */
    bsp_disp_set_invert(settings_load_display_invert());    /* 反色偏好 */
    bsp_disp_set_rotate180(settings_load_display_rotate()); /* 180° 旋转偏好 */
    bsp_disp_set_mirror_x(settings_load_display_mirror());  /* 水平镜像偏好 */
    boot_anim_play(bsp_lcd_push, bsp_delay_ms);   /* 「Umeko」开机动画（~2.5s） */
    ui_app_create();       /* 与 desktop 后端共享的同一份 UI 代码 */
#if BSP_HAS_BUTTONS
    bsp_gpio_buttons_set_handler(nav_button_forward);   /* 共享 keypad indev 已在上行建好 */
#endif
    bsp_set_brightness(settings_load_brightness());   /* 背光偏好（klipperscreen.conf） */
    bsp_set_screen_timeout(settings_load_screen_off());   /* 自动息屏（0=永不） */
    bsp_lvgl_unlock();

    /* WiFi/PHY 启动必须放在 UI 创建完成且释放 LVGL 锁之后。
       尤其 C3 是单核且使用原生 USB-Serial-JTAG：WiFi 启动时即使 USB
       链路短暂掉线，LVGL 任务也仍能获取锁并继续刷屏。 */
    bsp_wifi_init();

    /* WiFi 自动回连：有 network.conf 就用保存的凭据连接（Moonraker 客户端
       由 printer_model 的 2s 轮询在 WiFi 就绪后拉起） */
    wifi_conf_t wc;
    if (settings_load_wifi(&wc))
        bsp_wifi_connect(wc.ssid, wc.pass[0] ? wc.pass : NULL);

    debug_cli_start();       /* 串口调试命令行（help 查看） */
}
