#pragma once
/*
 * UI 图标的 LVGL A8 资源（tools/icongen/gen_icons.mjs 生成）
 * A8 = 仅 alpha 通道的白色图标，用 theme_img() 的 recolor 着色。
 * SVG 源文件在 svg/ 子目录。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_IMAGE_DECLARE(img_heater);      /* 28px 火焰：温度 */
LV_IMAGE_DECLARE(img_heater_56);   /* 56px 大屏 2x 变体 */
LV_IMAGE_DECLARE(img_nozzle_16);   /* 16px 喷嘴：标题栏 */
LV_IMAGE_DECLARE(img_nozzle_32);   /* 32px 喷嘴：温度面板 */
LV_IMAGE_DECLARE(img_bed_16);      /* 16px 热床：标题栏 */
LV_IMAGE_DECLARE(img_bed_32);      /* 32px 热床：温度面板 */
LV_IMAGE_DECLARE(img_move);        /* 28px 四向箭头 */
LV_IMAGE_DECLARE(img_move_56);     /* 56px 大屏变体 */
LV_IMAGE_DECLARE(img_extrude);     /* 28px 挤出 */
LV_IMAGE_DECLARE(img_extrude_56);
LV_IMAGE_DECLARE(img_files);       /* 28px 文件 */
LV_IMAGE_DECLARE(img_files_56);
LV_IMAGE_DECLARE(img_printer);     /* 28px 打印机 */
LV_IMAGE_DECLARE(img_printer_56);
LV_IMAGE_DECLARE(img_settings);    /* 28px 齿轮 */
LV_IMAGE_DECLARE(img_settings_56);
LV_IMAGE_DECLARE(img_wifi_4);      /* 18px 信号四档：强→弱 */
LV_IMAGE_DECLARE(img_wifi_3);
LV_IMAGE_DECLARE(img_wifi_2);
LV_IMAGE_DECLARE(img_wifi_1);
LV_IMAGE_DECLARE(img_link_off);    /* 16px 断链：状态卡 Moonraker 断连 */
LV_IMAGE_DECLARE(img_link_off_32); /* 32px 大屏变体 */
LV_IMAGE_DECLARE(img_link);        /* 16px 链接：状态卡已连接 */
LV_IMAGE_DECLARE(img_link_32);
LV_IMAGE_DECLARE(img_alert_circle);/* 16px 感叹号圆：状态卡 Klipper 异常 */
LV_IMAGE_DECLARE(img_alert_circle_32);
LV_IMAGE_DECLARE(img_globe_16);    /* 16px 地球：设置-语言行 */
LV_IMAGE_DECLARE(img_globe_32);    /* 32px 大屏变体 */
LV_IMAGE_DECLARE(img_swap_16);     /* 16px 双向箭头：Moonraker-切换打印机行 */
LV_IMAGE_DECLARE(img_swap_32);     /* 32px 大屏变体 */
LV_IMAGE_DECLARE(img_klipper_logo_56);  /* Klipper 标志：机器模式 / 打印机槽位 */
LV_IMAGE_DECLARE(img_klipper_logo_112); /* 大屏 2x 变体 */
LV_IMAGE_DECLARE(img_bambu_logo_56);    /* Bambu 标志：机器模式 */
LV_IMAGE_DECLARE(img_bambu_logo_112);   /* 大屏 2x 变体 */

#ifdef __cplusplus
}
#endif
