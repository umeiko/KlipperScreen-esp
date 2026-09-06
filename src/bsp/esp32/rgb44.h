/*
 * rgb44: 极简 RGB 并口屏驱动（ESP32-S3），传输模型照搬 IDF 4.4 的 esp_lcd_rgb_panel：
 *   - auto_next_frame = false：LCD 每刷完一帧自动停下
 *   - DMA 链一次性（末节点 next=NULL），一帧发完即 EOF 停
 *   - VSYNC_END 中断里做全量重启：gdma_reset + lcd stop + fifo reset +
 *     gdma_start(链头) + lcd start。LCD 处于停止态，重启没有任何竞态，
 *     单帧欠载/失步下一帧必然自愈。
 * 对比 IDF 5.5 官方驱动：stream 模式 LCD 永不停（auto_next_frame=true），
 * 欠载后错位永久保持（卷轴错位）；RESTART_IN_VSYNC 模式靠"跳过 FIFO_PRESERVE
 * 像素"猜 ISR 延迟，猜错就单帧错位（周期性抽动）。
 *
 * 只覆盖本项目需求：RGB565、单 fb（PSRAM）、stream 刷新、可选 vsync 回调。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rgb44_s *rgb44_handle_t;

typedef struct {
    uint32_t pclk_hz;
    uint16_t h_res, v_res;
    uint16_t hsync_pulse_width, hsync_back_porch, hsync_front_porch;
    uint16_t vsync_pulse_width, vsync_back_porch, vsync_front_porch;
    struct {
        uint32_t pclk_active_neg : 1;
        uint32_t pclk_idle_high : 1;
        uint32_t hsync_idle_low : 1;
        uint32_t vsync_idle_low : 1;
        uint32_t de_idle_high : 1;
    } flags;
} rgb44_timing_t;

typedef struct {
    rgb44_timing_t timing;
    int data_gpio_nums[16]; /* B0..B4, G0..G5, R0..R4 */
    int hsync_gpio_num;
    int vsync_gpio_num;
    int pclk_gpio_num;
    int de_gpio_num;
    int disp_gpio_num;      /* GPIO_NUM_NC 表示无 */
    bool disp_active_low;
    bool fb_in_psram;
    bool double_fb;         /* 双帧缓冲：vsync 换页，配合 LVGL DIRECT 消除撕裂 */
    void (*on_vsync)(void *user_ctx); /* 可选，ISR 上下文，须 IRAM 安全 */
    void *user_ctx;
} rgb44_config_t;

/* 建面板、配时钟/GPIO/DMA、启动持续刷新 */
esp_err_t rgb44_new(const rgb44_config_t *cfg, rgb44_handle_t *out);

/* 帧缓冲指针（RGB565，h_res*v_res*2 字节）；双缓冲时 idx ∈ {0,1} */
void *rgb44_fb(rgb44_handle_t h, int idx);

/* 请求下一帧 vsync 起扫描 fb 所在的缓冲（传 flush_cb 的 px_map 即可，
   按地址范围归属判断；新请求覆盖旧请求——丢帧不撕裂）。返回 true=命中某块 fb */
bool rgb44_show_fb(rgb44_handle_t h, const void *fb);

/* 阻塞等待 show_fb 请求的换页在某个 vsync 真正生效（DIRECT 模式 flush_cb 里
   必须在换页生效后再 flush_ready，否则 LVGL 的缓冲轮转和物理扫描脱钩，
   会往正在扫描的 fb 里渲染——小元素花屏）。返回 true=换页已生效 */
bool rgb44_wait_swap(rgb44_handle_t h, uint32_t timeout_ms);

/* 当前正在扫描的 fb 索引（诊断用） */
int rgb44_cur(rgb44_handle_t h);

/* disp GPIO 开关屏（无 disp GPIO 返回 ESP_ERR_NOT_SUPPORTED） */
esp_err_t rgb44_disp_on_off(rgb44_handle_t h, bool on);

/* 诊断：快照并清零统计。vs_count=期间vsync帧数；min/max_us=相邻vsync间隔
   最小/最大（us）；late=vsync到达时DMA还没发完上一帧的次数（欠载帧） */
void rgb44_stats(rgb44_handle_t h, uint32_t *vs_count, uint32_t *min_us, uint32_t *max_us, uint32_t *late);

#ifdef __cplusplus
}
#endif
