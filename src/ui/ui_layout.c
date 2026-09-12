#include "ui_layout.h"
#include "assets/icons.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"   /* CONFIG_BOARD_* 板型宏（裁剪全表字体尺寸档用） */
#endif

/* CJK 子集字体（tools/fontgen/gen_fonts.py 生成）；14/16 另有 _cmp 压缩变体：
   CYD（4MB flash，屏小字少）用压缩版省 flash，desktop 用不压缩版省渲染 CPU。
   28/32 另有 _min 最小子集变体（仅 UI 字面量，几百字）：JC8048 用——全表 5.4MB
   字形走 XIP cache 读，表大 cache 局部性差，渲染文本的 flash 突发在 MSPI 上
   与 EDMA 扫描争抢（滑动抽动的嫌疑变量，排障期用最小集恢复原状验证）；
   代价是文件名/SSID 里表外汉字显示方框。改回全表：把 UI_FONT_MIN 置 0 */
LV_FONT_DECLARE(font_cjk_10);
LV_FONT_DECLARE(font_cjk_12);
LV_FONT_DECLARE(font_cjk_14);
LV_FONT_DECLARE(font_cjk_16);
LV_FONT_DECLARE(font_cjk_10_cmp);
LV_FONT_DECLARE(font_cjk_12_cmp);
LV_FONT_DECLARE(font_cjk_14_cmp);
LV_FONT_DECLARE(font_cjk_16_cmp);
LV_FONT_DECLARE(font_cjk_28);
LV_FONT_DECLARE(font_cjk_32);
LV_FONT_DECLARE(font_cjk_28_min);
LV_FONT_DECLARE(font_cjk_32_min);

static int   scr_w = 320;
static int   scr_h = 240;
static float scale_f = 1.0f;

void ui_layout_init(void)
{
    lv_display_t *d = lv_display_get_default();
    if (!d) return;
    scr_w = lv_display_get_horizontal_resolution(d);
    scr_h = lv_display_get_vertical_resolution(d);
    scale_f = (float)scr_h / 240.0f;
    /* 不钳下限：160x128（scale≈0.53）等小屏按同比例缩小几何，
       字体走 small() 档（ui_font_*），避免等比缩到不可读的 7px */
}

int   ui_scr_w(void)  { return scr_w; }
int   ui_scr_h(void)  { return scr_h; }
float ui_scale(void)  { return scale_f; }

int ui_px(int v)
{
    return (int)(v * scale_f + (v >= 0 ? 0.5f : -0.5f));
}

int ui_gap(int v)
{
    /* 间距次线性：scale 2.0 → 1.5x。控件尺寸照 ui_px 等比放大，
       但间距等比放大后（800x480 上 96px 列距）视觉上过于空旷 */
    float s = 1.0f + (scale_f - 1.0f) * 0.5f;
    return (int)(v * s + (v >= 0 ? 0.5f : -0.5f));
}

int ui_content_w(void)
{
    return scr_w - 2 * ui_px(8);
}

static int big(void)   { return scale_f >= 2.0f; }
static int small(void) { return scale_f < 1.0f; }

/* 字号档选择：ESP32 按板型在预处理期定死，未用的全表字体直接被链接器丢掉
   （CYD/SZP 只链 14/16，JC8048 只链 28/32，ec11_knob_esp32 只链 10/12 ——
   4MB/16MB flash 都放得下 GB2312 全表）；desktop 走运行时 big()/small()
   （KLIPPER_RES 可切分辨率，三档都要）。 */
#if defined(CONFIG_BOARD_CYD_2432S028R)
#define UI_FONT_BIG 0
#elif defined(CONFIG_BOARD_E32R35T)
#define UI_FONT_BIG 0
#elif defined(CONFIG_BOARD_EC11_KNOB_MINIMAL)
#define UI_FONT_BIG 0
#elif defined(CONFIG_BOARD_ESP32S3_JLC_SZP)
#define UI_FONT_BIG 0   /* 320x240 同 CYD：只链 14/16 压缩档全表 */
#elif defined(CONFIG_BOARD_EC11_KNOB_ESP32)
#define UI_FONT_SMALL 1   /* 160x128：几何等比缩小，字体走 10/12 小屏档 */
#elif defined(CONFIG_BOARD_JC8048W550)
#define UI_FONT_BIG 1
#define UI_FONT_MIN 1   /* 最小子集（排障：缩小 flash 字形表的 XIP 流量），置 0 回全表 */
#endif

/* JC8048：最小子集优先，未定义 UI_FONT_MIN 时默认全表 */
#if defined(UI_FONT_MIN) && UI_FONT_MIN
#define UI_FONT_28 font_cjk_28_min
#define UI_FONT_32 font_cjk_32_min
#else
#define UI_FONT_28 font_cjk_28
#define UI_FONT_32 font_cjk_32
#endif

const lv_font_t *ui_font_s(void)
{
#if defined(UI_FONT_SMALL)
    return &font_cjk_10_cmp;
#elif defined(UI_FONT_BIG)
    return UI_FONT_BIG ? &UI_FONT_28 : &font_cjk_14_cmp;
#else
    return small() ? &font_cjk_10 : big() ? &font_cjk_28 : &font_cjk_14;
#endif
}

const lv_font_t *ui_font_m(void)
{
#if defined(UI_FONT_SMALL)
    return &font_cjk_12_cmp;
#elif defined(UI_FONT_BIG)
    return UI_FONT_BIG ? &UI_FONT_32 : &font_cjk_16_cmp;
#else
    return small() ? &font_cjk_12 : big() ? &font_cjk_32 : &font_cjk_16;
#endif
}

const lv_font_t *ui_font_l(void)
{
#if defined(UI_FONT_SMALL)
    return &lv_font_montserrat_12;
#elif defined(UI_FONT_BIG)
    return UI_FONT_BIG ? &lv_font_montserrat_48 : &lv_font_montserrat_24;
#else
    return small() ? &lv_font_montserrat_12
         : big()   ? &lv_font_montserrat_48 : &lv_font_montserrat_24;
#endif
}

const lv_font_t *ui_font_xl(void)
{
#if defined(UI_FONT_SMALL)
    return &lv_font_montserrat_16;
#elif defined(UI_FONT_BIG)
    return UI_FONT_BIG ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
#else
    return small() ? &lv_font_montserrat_16
         : big()   ? &lv_font_montserrat_48 : &lv_font_montserrat_28;
#endif
}

const lv_font_t *ui_font_icon(void)
{
#if defined(UI_FONT_SMALL)
    return &lv_font_montserrat_12;
#elif defined(UI_FONT_BIG)
    return UI_FONT_BIG ? &lv_font_montserrat_32 : &lv_font_montserrat_16;
#else
    return small() ? &lv_font_montserrat_12
         : big()   ? &lv_font_montserrat_32 : &lv_font_montserrat_16;
#endif
}

/* 小屏(160x128) 0.45x 图标映射：base → _sm 变体（tools/icongen 生成） */
static const struct { const lv_image_dsc_t *base, *sm; } icon_sm_map[] = {
    { &img_heater,          &img_heater_sm },
    { &img_nozzle_16,       &img_nozzle_16_sm },
    { &img_bed_16,          &img_bed_16_sm },
    { &img_nozzle_32,       &img_nozzle_32_sm },
    { &img_bed_32,          &img_bed_32_sm },
    { &img_move,            &img_move_sm },
    { &img_extrude,         &img_extrude_sm },
    { &img_files,           &img_files_sm },
    { &img_printer,         &img_printer_sm },
    { &img_settings,        &img_settings_sm },
    { &img_wifi_4,          &img_wifi_4_sm },
    { &img_wifi_3,          &img_wifi_3_sm },
    { &img_wifi_2,          &img_wifi_2_sm },
    { &img_wifi_1,          &img_wifi_1_sm },
    { &img_link_off,        &img_link_off_sm },
    { &img_link,            &img_link_sm },
    { &img_alert_circle,    &img_alert_circle_sm },
    { &img_globe_16,        &img_globe_16_sm },
    { &img_swap_16,         &img_swap_16_sm },
    { &img_klipper_logo_56, &img_klipper_logo_56_sm },
    { &img_bambu_logo_56,   &img_bambu_logo_56_sm },
};

static const lv_image_dsc_t *icon_sm(const lv_image_dsc_t *base)
{
    for (size_t i = 0; i < sizeof(icon_sm_map) / sizeof(icon_sm_map[0]); i++)
        if (icon_sm_map[i].base == base) return icon_sm_map[i].sm;
    return base;
}

const lv_image_dsc_t *ui_icon(const lv_image_dsc_t *i16, const lv_image_dsc_t *i32)
{
#if defined(UI_FONT_SMALL)
    (void)i32;
    return icon_sm(i16);   /* 小屏恒用 0.45x 变体（无映射则用原图） */
#elif defined(UI_FONT_BIG)
    return (UI_FONT_BIG && i32) ? i32 : i16;
#else
    if (small()) return icon_sm(i16);
    return (big() && i32) ? i32 : i16;
#endif
}
