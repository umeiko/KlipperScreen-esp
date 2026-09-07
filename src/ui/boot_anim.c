/*
 * 「Umeko」霓虹描边开机动画 —— 平台无关核心（参考 .reff/boot_animation 的 TFT_eSPI 版）。
 * 渲染到 RAM 行缓冲（屏宽 x 64 行分带），由平台回调推屏，避免逐像素 SPI 开销。
 * 画面尺寸/Logo 缩放从默认 display 分辨率推导（320x240 与 800x480 通用）。
 */
#include "boot_anim.h"
#include "assets/boot_logo_path.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Logo 数据以 320x240 画面（中心 160,120、Logo 约 280x240）为基准生成，
 * 运行时按屏幕高度等比放大并居中 */
static int scr_w = 320, scr_h = 240;
static int logo_k = 100;         /* Logo 缩放（%）：scr_h/240*100 */
static int neon_cx = 160, neon_cy = 120;

/* 以画面中心为原点的缩放变换（s 为百分比）。不钳位：大景别时 Logo 超屏，
 * 越界点由 zfb_plot 丢弃，形成镜头太近、后拉逐渐入画的效果 */
static inline int neon_sx(int x, int s) { return neon_cx + (x - 160) * s * logo_k / 10000; }
static inline int neon_sy(int y, int s) { return neon_cy + (y - 120) * s * logo_k / 10000; }

/* 行缓冲：Logo 区为屏幕纵向中段 2/3，按 40 行分带。
 * 40 行 = 两块 SPI 屏 max_transfer_sz 恰好容纳（480*40*2=38400 ≤ 38408，
 * 320*40*2=25600 ≤ 25608）；64 行时在 E32R35T 上超限，draw_bitmap 静默失败，
 * 开机动画只剩一条黑带 + 白屏 */
#define ZFB_H  40
static int zfb_y0 = 40, zfb_y1 = 200;

static uint16_t *zfb;            /* 播放时 malloc，结束释放 */
static int       zfb_y_base;     /* 当前带在屏幕上的 y 起点 */

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void zfb_plot(int x, int y, uint16_t c)
{
    if (x < 0 || x >= scr_w) return;
    int ry = y - zfb_y_base;
    if (ry < 0 || ry >= ZFB_H) return;
    zfb[ry * scr_w + x] = c;
}

/* 亮芯 + 四邻域光晕（霓虹灯管 bloom）；dim: 4 最亮 → 0 熄灭 */
static inline void neon_dot(int x, int y, int dim)
{
    zfb_plot(x,     y,     rgb565(215 * dim / 4, 250 * dim / 4, 255 * dim / 4));
    zfb_plot(x - 1, y,     rgb565(55 * dim / 4, 150 * dim / 4, 200 * dim / 4));
    zfb_plot(x + 1, y,     rgb565(55 * dim / 4, 150 * dim / 4, 200 * dim / 4));
    zfb_plot(x,     y - 1, rgb565(55 * dim / 4, 150 * dim / 4, 200 * dim / 4));
    zfb_plot(x,     y + 1, rgb565(55 * dim / 4, 150 * dim / 4, 200 * dim / 4));
}

/* 完整描边（渐变洗入后勾边 / 渐暗用），dim=4 全亮 */
static void neon_outline(int s, int dim)
{
    for (int i = 0; i < LOGO_PATH_LEN; i++)
        neon_dot(neon_sx(logo_path_x[i], s), neon_sy(logo_path_y[i], s), dim);
}

/* Logo 剪影竖向渐变填充（顶部黄 → 底部红，1/3 高处镜面高光带），level 0~5 */
static void neon_fill(int s, int level)
{
    for (int i = 0; i < LOGO_FILL_LEN; i++) {
        float g = (float)(logo_fill_y[i] - LOGO_TOP_Y) / (LOGO_BOT_Y - LOGO_TOP_Y);
        float sd = g - 0.32f; if (sd < 0) sd = -sd;
        int spec = sd < 0.18f ? (int)(110 * (1.0f - sd / 0.18f)) : 0;
        int r = 255 + spec;                  if (r > 255) r = 255;
        int gg = 210 - (int)(165 * g) + spec; if (gg > 255) gg = 255;
        int b = 40 - (int)(10 * g) + spec * 7 / 10;
        r = r * level / 5; gg = gg * level / 5; b = b * level / 5;
        int x0 = neon_sx(logo_fill_x0[i], s), x1 = neon_sx(logo_fill_x1[i], s);
        int y  = neon_sy(logo_fill_y[i], s);
        if (x0 < 0) x0 = 0;
        if (x1 >= scr_w) x1 = scr_w - 1;
        uint16_t c = rgb565(r, gg, b);
        for (int x = x0; x <= x1; x++) zfb_plot(x, y, c);
    }
}

/* 清屏：整幅黑（分带覆盖全高） */
static void push_black(boot_anim_push_t push)
{
    memset(zfb, 0, (size_t)scr_w * ZFB_H * sizeof(uint16_t));
    for (int y = 0; y < scr_h; y += ZFB_H) {
        int hh = scr_h - y; if (hh > ZFB_H) hh = ZFB_H;
        push(0, y, scr_w, hh, zfb);
    }
}

void boot_anim_play(boot_anim_push_t push, boot_anim_delay_t dly)
{
    if (!push) return;

    /* 画面几何从默认 display 分辨率推导（此时 LVGL display 已建好） */
    lv_display_t *d = lv_display_get_default();
    if (d) {
        scr_w = lv_display_get_horizontal_resolution(d);
        scr_h = lv_display_get_vertical_resolution(d);
    }
    neon_cx = scr_w / 2;
    neon_cy = scr_h / 2;
    logo_k  = scr_h * 100 / 240;
    zfb_y0  = scr_h / 6;
    zfb_y1  = scr_h * 5 / 6;

    zfb = malloc((size_t)scr_w * ZFB_H * sizeof(uint16_t));
    if (!zfb) return;
    push_black(push);

    const uint16_t core_c = rgb565(215, 250, 255);
    const uint16_t halo_c = rgb565(55, 150, 200);

    /* 第一阶段：描边 + 镜头后拉（160% → 88%），亮头每段同步行进 */
    const int ZOOM_FRAMES = 45;
    const int S_START = 160, S_END = 88;
    for (int f = 0; f < ZOOM_FRAMES; f++) {
        float t = (float)f / (ZOOM_FRAMES - 1);
        float ez = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   /* 缩放缓出 */
        int sc = S_START - (int)((S_START - S_END) * ez);
        float et = t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;  /* 描边缓入缓出 */
        for (zfb_y_base = zfb_y0; zfb_y_base < zfb_y1; zfb_y_base += ZFB_H) {
            int hh = zfb_y1 - zfb_y_base;
            if (hh > ZFB_H) hh = ZFB_H;
            memset(zfb, 0, (size_t)scr_w * hh * sizeof(uint16_t));
            for (int s = 0; s < LOGO_SEG_CNT; s++) {
                int off = logo_seg_off[s];
                int n = logo_seg_off[s + 1] - off;
                int head = (int)(n * et);
                if (head > n - 1) head = n - 1;
                for (int i = 0; i <= head; i++) {
                    int x = neon_sx(logo_path_x[off + i], sc);
                    int y = neon_sy(logo_path_y[off + i], sc);
                    zfb_plot(x, y, core_c);
                    zfb_plot(x - 1, y, halo_c);
                    zfb_plot(x + 1, y, halo_c);
                    zfb_plot(x, y - 1, halo_c);
                    zfb_plot(x, y + 1, halo_c);
                }
                zfb_plot(neon_sx(logo_path_x[off + head], sc),
                         neon_sy(logo_path_y[off + head], sc), 0xFFFF);   /* 亮头纯白 */
            }
            push(0, zfb_y_base, scr_w, hh, zfb);
        }
        if (dly) dly(25);
    }
    if (dly) dly(150);   /* 落定稍停 */

    /* 第二阶段：洗入渐变（88% 景别），每档后重描轮廓勾边 */
    for (int lvl = 1; lvl <= 5; lvl++) {
        for (zfb_y_base = zfb_y0; zfb_y_base < zfb_y1; zfb_y_base += ZFB_H) {
            int hh = zfb_y1 - zfb_y_base;
            if (hh > ZFB_H) hh = ZFB_H;
            memset(zfb, 0, (size_t)scr_w * hh * sizeof(uint16_t));
            neon_fill(S_END, lvl);
            neon_outline(S_END, 4);
            push(0, zfb_y_base, scr_w, hh, zfb);
        }
        if (dly) dly(30);
    }
    if (dly) dly(300);   /* 定格 */

    /* 整体渐暗 */
    for (int d = 4; d >= 0; d--) {
        for (zfb_y_base = zfb_y0; zfb_y_base < zfb_y1; zfb_y_base += ZFB_H) {
            int hh = zfb_y1 - zfb_y_base;
            if (hh > ZFB_H) hh = ZFB_H;
            memset(zfb, 0, (size_t)scr_w * hh * sizeof(uint16_t));
            neon_fill(S_END, d);
            neon_outline(S_END, d);
            push(0, zfb_y_base, scr_w, hh, zfb);
        }
        if (dly) dly(25);
    }
    push_black(push);

    free(zfb);
    zfb = NULL;
}
