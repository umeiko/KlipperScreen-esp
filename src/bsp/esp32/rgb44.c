/* 见 rgb44.h 头注释：IDF 4.4 传输模型的 ESP32-S3 / IDF 5.5 实现
 * v2：支持双帧缓冲 + vsync 换页（DIRECT 渲染模式下消除撕裂）
 * 排障全程见 docs/jc8048w550-rgb-display-guide.md */
#include "sdkconfig.h"
#if CONFIG_BOARD_JC8048W550

#include "rgb44.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_clk_tree.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_psram.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/lcd_hal.h"
#include "hal/lcd_ll.h"
#include "hal/lcd_types.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/lcd_periph.h"
#include "soc/soc_caps.h"

/* esp_lcd_common.h 里的 LCD_CLOCK_SRC_ATOMIC 是组件私有头，这里等价展开 */
#if SOC_RCC_IS_ATOMIC
#define RGB44_RCC_ATOMIC() PERIPH_RCC_ATOMIC()
#else
#define RGB44_RCC_ATOMIC()
#endif

#define RGB44_DMA_NODE_MAX 4095 /* LCD DMA 描述符单节点最大字节数 */
#define RGB44_PANEL_ID    0     /* S3 只有一个 RGB 面板槽位 */

struct rgb44_s {
    lcd_hal_context_t hal;
    gdma_channel_handle_t dma_chan;
    gdma_link_list_handle_t link[2]; /* 每块 fb 一条 DMA 链 */
    intr_handle_t intr;
    uint8_t *fb[2];
    int num_fbs;
    size_t fb_size;
    rgb44_config_t cfg;
    int cur;                 /* 正在扫描的 fb 索引 */
    volatile int pending;    /* 请求下次 vsync 换到的 fb 索引，-1=无 */
    SemaphoreHandle_t swap_sem; /* 换页生效信号：仅在实际换页的那个 vsync 给出 */
    /* 诊断统计（ISR 上下文更新） */
    volatile uint32_t vs_count;      /* vsync 总数 */
    volatile uint32_t vs_min_cyc;    /* 相邻 vsync 最小间隔（CCOUNT） */
    volatile uint32_t vs_max_cyc;    /* 相邻 vsync 最大间隔 */
    volatile uint32_t vs_last;       /* 上次 vsync 的 CCOUNT */
    volatile uint32_t dma_late;      /* vsync 到了但 DMA 还没发完上一帧的次数 */
    volatile uint32_t eof_seen;      /* 本帧 DMA EOF 是否已触发 */
};

/* 每帧 VSYNC_END：LCD 已停（auto_next_frame=false），安全地全量重启传输。
   返回 true = 发生了换页且唤醒了等待任务，需要 yield */
static IRAM_ATTR bool rgb44_start_transmission(rgb44_handle_t p)
{
    bool yield = false;
    /* 换页请求在帧边界生效：只换 gdma_start 的链头，零拷贝 */
    int pend = p->pending;
    if (pend >= 0) {
        p->cur = pend;
        p->pending = -1;
        if (p->swap_sem) {
            BaseType_t hp = pdFALSE;
            xSemaphoreGiveFromISR(p->swap_sem, &hp);
            yield = (hp == pdTRUE);
        }
    }
    gdma_reset(p->dma_chan);
    lcd_ll_stop(p->hal.dev);
    lcd_ll_fifo_reset(p->hal.dev);
    gdma_start(p->dma_chan, (intptr_t)gdma_link_get_head_addr(p->link[p->cur]));
    /* 给 DMA 1us 把数据压进 LCD FIFO（PCLK 高时需要） */
    esp_rom_delay_us(1);
    lcd_ll_start(p->hal.dev);
    return yield;
}

static IRAM_ATTR bool rgb44_dma_eof_cb(gdma_channel_handle_t chan, gdma_event_data_t *ev, void *user)
{
    (void)chan; (void)ev;
    ((rgb44_handle_t)user)->eof_seen = 1;
    return false;
}

static IRAM_ATTR void rgb44_isr(void *args)
{
    rgb44_handle_t p = (rgb44_handle_t)args;
    uint32_t st = lcd_ll_get_interrupt_status(p->hal.dev);
    lcd_ll_clear_interrupt_status(p->hal.dev, st);
    if (st & LCD_LL_EVENT_VSYNC_END) {
        uint32_t now = esp_cpu_get_cycle_count();
        if (p->vs_last) {
            uint32_t d = now - p->vs_last;
            if (p->vs_count == 0 || d < p->vs_min_cyc) p->vs_min_cyc = d;
            if (d > p->vs_max_cyc) p->vs_max_cyc = d;
            p->vs_count++;
        }
        p->vs_last = now;
        if (!p->eof_seen) p->dma_late++;
        p->eof_seen = 0;
        if (p->cfg.on_vsync) {
            p->cfg.on_vsync(p->cfg.user_ctx);
        }
        if (rgb44_start_transmission(p)) {
            portYIELD_FROM_ISR();
        }
    }
}

/* 快照并清零统计。周期 us 按 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 换算 */
void rgb44_stats(rgb44_handle_t p, uint32_t *vs_count, uint32_t *min_us, uint32_t *max_us, uint32_t *dma_late)
{
    /* 不在临界区，够用于诊断 */
    if (!p) return;
    uint32_t mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    if (vs_count) *vs_count = p->vs_count;
    if (min_us) *min_us = p->vs_min_cyc / mhz;
    if (max_us) *max_us = p->vs_max_cyc / mhz;
    if (dma_late) *dma_late = p->dma_late;
    p->vs_count = 0;
    p->vs_min_cyc = 0;
    p->vs_max_cyc = 0;
    p->dma_late = 0;
}

static esp_err_t rgb44_config_gpio(rgb44_handle_t p)
{
    const rgb44_config_t *cfg = &p->cfg;
    const int *sigs = lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].data_sigs;

    for (int i = 0; i < 16; i++) {
        if (cfg->data_gpio_nums[i] < 0) return ESP_ERR_INVALID_ARG;
        (void)gpio_func_sel(cfg->data_gpio_nums[i], PIN_FUNC_GPIO);
        gpio_set_direction(cfg->data_gpio_nums[i], GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(cfg->data_gpio_nums[i], sigs[i], false, false);
    }
    if (cfg->hsync_gpio_num >= 0) {
        (void)gpio_func_sel(cfg->hsync_gpio_num, PIN_FUNC_GPIO);
        gpio_set_direction(cfg->hsync_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(cfg->hsync_gpio_num,
            lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].hsync_sig, false, false);
    }
    if (cfg->vsync_gpio_num >= 0) {
        (void)gpio_func_sel(cfg->vsync_gpio_num, PIN_FUNC_GPIO);
        gpio_set_direction(cfg->vsync_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(cfg->vsync_gpio_num,
            lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].vsync_sig, false, false);
    }
    if (cfg->pclk_gpio_num < 0) return ESP_ERR_INVALID_ARG;
    (void)gpio_func_sel(cfg->pclk_gpio_num, PIN_FUNC_GPIO);
    gpio_set_direction(cfg->pclk_gpio_num, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(cfg->pclk_gpio_num,
        lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].pclk_sig, false, false);
    if (cfg->de_gpio_num >= 0) {
        (void)gpio_func_sel(cfg->de_gpio_num, PIN_FUNC_GPIO);
        gpio_set_direction(cfg->de_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(cfg->de_gpio_num,
            lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].de_sig, false, false);
    }
    if (cfg->disp_gpio_num >= 0) {
        (void)gpio_func_sel(cfg->disp_gpio_num, PIN_FUNC_GPIO);
        gpio_set_direction(cfg->disp_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(cfg->disp_gpio_num, SIG_GPIO_OUT_IDX, false, false);
    }
    return ESP_OK;
}

esp_err_t rgb44_new(const rgb44_config_t *cfg, rgb44_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, "rgb44", "invalid arg");

    /* lcd_ll_enable_interrupt 的宏包装要求作用域内声明该变量（强制临界区检查） */
    int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));

    rgb44_handle_t p = calloc(1, sizeof(struct rgb44_s));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, "rgb44", "no mem");
    p->cfg = *cfg;
    p->pending = -1;
    p->swap_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(p->swap_sem, ESP_ERR_NO_MEM, "rgb44", "no sem");

    const periph_module_t mod = lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].module;
    const int irq_id = lcd_periph_rgb_signals.panels[RGB44_PANEL_ID].irq_id;
    esp_err_t ret = ESP_OK;

    /* 帧缓冲：RGB565，双缓冲可选 */
    p->fb_size = (size_t)cfg->timing.h_res * cfg->timing.v_res * 2;
    p->num_fbs = cfg->double_fb ? 2 : 1;
    bool use_psram = cfg->fb_in_psram && esp_psram_is_initialized();
    for (int i = 0; i < p->num_fbs; i++) {
        if (use_psram) {
            p->fb[i] = heap_caps_aligned_calloc(64, 1, p->fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        } else {
            p->fb[i] = heap_caps_aligned_calloc(4, 1, p->fb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        ESP_GOTO_ON_FALSE(p->fb[i], ESP_ERR_NO_MEM, err, "rgb44", "no mem for fb%d", i);
    }

    /* 使能外设 + HAL */
    periph_module_enable(mod);
    periph_module_reset(mod);
    lcd_hal_init(&p->hal, RGB44_PANEL_ID);
    lcd_ll_enable_clock(p->hal.dev, true);

    /* 时钟源：PLL160M */
    uint32_t src_hz = 0;
    ESP_GOTO_ON_ERROR(
        esp_clk_tree_src_get_freq_hz((soc_module_clk_t)LCD_CLK_SRC_PLL160M,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_hz),
        err, "rgb44", "get clk src freq failed");
    ESP_GOTO_ON_ERROR(esp_clk_tree_enable_src((soc_module_clk_t)LCD_CLK_SRC_PLL160M, true),
                      err, "rgb44", "enable clk src failed");
    lcd_ll_select_clk_src(p->hal.dev, LCD_CLK_SRC_PLL160M);

    /* VSYNC_END 中断（LCD_CAM 与 camera 共享中断源） */
    ret = esp_intr_alloc_intrstatus(irq_id, ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_SHARED,
                                    (uint32_t)lcd_ll_get_interrupt_status_reg(p->hal.dev),
                                    LCD_LL_EVENT_VSYNC_END, rgb44_isr, p, &p->intr);
    ESP_GOTO_ON_ERROR(ret, err, "rgb44", "alloc intr failed");
    lcd_ll_enable_interrupt(p->hal.dev, LCD_LL_EVENT_VSYNC_END, false);
    lcd_ll_clear_interrupt_status(p->hal.dev, UINT32_MAX);

    /* GPIO */
    ESP_GOTO_ON_ERROR(rgb44_config_gpio(p), err, "rgb44", "gpio config failed");

    /* GDMA 通道（优先级拉满，减少与别的 DMA 通道的仲裁损失） */
    gdma_channel_alloc_config_t chan_cfg = { .direction = GDMA_CHANNEL_DIRECTION_TX };
    ESP_GOTO_ON_ERROR(gdma_new_ahb_channel(&chan_cfg, &p->dma_chan), err, "rgb44", "alloc gdma failed");
    gdma_connect(p->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
    gdma_apply_strategy(p->dma_chan, &(gdma_strategy_config_t){ .eof_till_data_popped = false });
    ESP_GOTO_ON_ERROR(gdma_config_transfer(p->dma_chan, &(gdma_transfer_config_t){
                          .max_data_burst_size = 64, .access_ext_mem = use_psram }),
                      err, "rgb44", "config gdma failed");
    gdma_set_priority(p->dma_chan, 3);
    ESP_GOTO_ON_ERROR(gdma_register_tx_event_callbacks(p->dma_chan, &(gdma_tx_event_callbacks_t){
                          .on_trans_eof = rgb44_dma_eof_cb }, p),
                      err, "rgb44", "register eof cb failed");

    /* DMA 链：一次性，末节点 next=NULL（4.4 同款，区别于 5.5 的环形链）。
       注意：挂载时每个节点的 buffer 起点要对齐，单节点容量 = 4095 向下对齐到
       buffer_alignment（64）= 4032 字节，节点数要按此容量算 */
    uint32_t node_cap = RGB44_DMA_NODE_MAX & ~63U;
    uint32_t num_nodes = (p->fb_size + node_cap - 1) / node_cap;
    for (int i = 0; i < p->num_fbs; i++) {
        ESP_GOTO_ON_ERROR(gdma_new_link_list(&(gdma_link_list_config_t){
                              .num_items = num_nodes, .flags = { .check_owner = false } },
                          &p->link[i]), err, "rgb44", "new link list failed");
        ESP_GOTO_ON_ERROR(gdma_link_mount_buffers(p->link[i], 0, &(gdma_buffer_mount_config_t){
                              .buffer = p->fb[i],
                              .buffer_alignment = use_psram ? 64 : 4,
                              .length = p->fb_size,
                              .flags = { .mark_eof = 1, .mark_final = GDMA_FINAL_LINK_TO_NULL },
                          }, 1, NULL), err, "rgb44", "mount fb failed");
    }

    /* LCD 控制器配置（同 5.5 rgb_panel_init，唯二区别在函数尾部） */
    const rgb44_timing_t *t = &cfg->timing;
    hal_utils_clk_div_t clk_div = {};
    lcd_hal_cal_pclk_freq(&p->hal, src_hz, t->pclk_hz, &clk_div);
    RGB44_RCC_ATOMIC() {
        lcd_ll_set_group_clock_coeff(p->hal.dev, clk_div.integer, clk_div.denominator, clk_div.numerator);
    }
    lcd_ll_set_clock_idle_level(p->hal.dev, t->flags.pclk_idle_high);
    lcd_ll_set_pixel_clock_edge(p->hal.dev, t->flags.pclk_active_neg);
    lcd_ll_enable_rgb_mode(p->hal.dev, true);
    lcd_ll_set_dma_read_stride(p->hal.dev, 16);
    lcd_ll_set_phase_cycles(p->hal.dev, 0, 0, 1); /* 仅数据相位 */
    lcd_ll_enable_output_always_on(p->hal.dev, true);
    lcd_ll_set_idle_level(p->hal.dev, !t->flags.hsync_idle_low, !t->flags.vsync_idle_low, t->flags.de_idle_high);
    lcd_ll_set_blank_cycles(p->hal.dev, 1, 1);
    lcd_ll_set_horizontal_timing(p->hal.dev, t->hsync_pulse_width, t->hsync_back_porch, t->h_res, t->hsync_front_porch);
    lcd_ll_set_vertical_timing(p->hal.dev, t->vsync_pulse_width, t->vsync_back_porch, t->v_res, t->vsync_front_porch);
    lcd_ll_enable_output_hsync_in_porch_region(p->hal.dev, true);
    lcd_ll_set_hsync_position(p->hal.dev, 0);
    /* 核心区别 1：帧尾 LCD 自动停（5.5 stream 模式是 true，永不停） */
    lcd_ll_enable_auto_next_frame(p->hal.dev, false);
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(p->hal.dev, LCD_LL_EVENT_VSYNC_END, true);
    }
    esp_intr_enable(p->intr);

    /* 启动持续刷新：之后每帧由 VSYNC_END ISR 全量重启 */
    rgb44_start_transmission(p);

    *out = p;
    return ESP_OK;

err:
    for (int i = 0; i < 2; i++) {
        if (p->fb[i]) free(p->fb[i]);
    }
    free(p);
    return ret ? ret : ESP_FAIL;
}

void *rgb44_fb(rgb44_handle_t h, int idx)
{
    if (idx < 0 || idx >= h->num_fbs) return NULL;
    return h->fb[idx];
}

bool rgb44_show_fb(rgb44_handle_t h, const void *fb)
{
    /* px_map 可能指向 fb 内部偏移，按范围判断归属 */
    const uint8_t *ptr = (const uint8_t *)fb;
    for (int i = 0; i < h->num_fbs; i++) {
        if (ptr >= h->fb[i] && ptr < h->fb[i] + h->fb_size) {
            h->pending = i; /* 下一帧 vsync 生效；新请求覆盖旧请求（丢帧不撕裂） */
            return true;
        }
    }
    return false;
}

bool rgb44_wait_swap(rgb44_handle_t h, uint32_t timeout_ms)
{
    if (!h || !h->swap_sem) return true;
    /* 信号量只在"实际换页"的 vsync 给出，拿到即换页已生效 */
    return xSemaphoreTake(h->swap_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

int rgb44_cur(rgb44_handle_t h)
{
    return h ? h->cur : -1;
}

esp_err_t rgb44_disp_on_off(rgb44_handle_t h, bool on)
{
    ESP_RETURN_ON_FALSE(h->cfg.disp_gpio_num >= 0, ESP_ERR_NOT_SUPPORTED, "rgb44", "no disp gpio");
    gpio_set_level(h->cfg.disp_gpio_num, on == !h->cfg.disp_active_low);
    return ESP_OK;
}

#endif /* CONFIG_BOARD_JC8048W550 */
