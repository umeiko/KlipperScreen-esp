# JC8048W550 RGB 屏显示排坑大指南

> 适用对象：ESP32-S3 + RGB 并口屏（本文以 JC8048W550：5 寸 800×480 ST7262 + GT911 触摸为例）
> 关键词：画面抽动、X 方向撕裂、卷轴错位、小元素花屏/不更新、LVGL DIRECT、GDMA、cache 一致性
> 状态：**已彻底解决**。最终方案 = 自研 rgb44 驱动（IDF 4.4 传输模型）+ LVGL DIRECT 双缓冲 + vsync 换页同步 + cache 回写。

---

## 0. TL;DR——如果你只想抄结论

在 ESP32-S3 / IDF 5.5.x 上驱动 800×480 RGB 屏 + LVGL 9.3，要同时满足下面**四条**才能得到完全稳定的画面，缺任何一条都有对应的特征故障：

| # | 必须做的事 | 缺了它的症状 |
|---|-----------|-------------|
| 1 | 传输模型用 4.4 式：`auto_next_frame=false` + 一次性 DMA 链 + vsync 全量重启 | 欠载后卷轴错位永久保持 / 周期性抽动 |
| 2 | LVGL `DIRECT` 双 fb 直渲（PSRAM），零 memcpy | 滑动时大面积 memcpy 挤爆 MSPI 总线 → 欠载帧 → 抽动 |
| 3 | flush_cb 只在**最后一次** flush 请求换页，并**阻塞等 vsync 换页生效**再 `flush_ready` | 往正在扫描的 fb 里渲染 → 小元素花屏 |
| 4 | 换页前 `esp_cache_msync(fb, 整帧, ESP_CACHE_MSYNC_FLAG_DIR_C2M)` | 小元素"点了没变、微微花，大滚动才正常" |

参考实现：`tmp/lvtest/main/rgb44.c`（~300 行）+ `tmp/lvtest/main/main.c`（LVGL 对接）。

---

## 1. 症状编年史（排障过程中逐层剥开的四层面纱）

这个问题不是单一 bug，而是**四个独立问题叠加**，每层修掉后露出下一层，极具迷惑性：

1. **卷轴错位**：画面从中间断开，左右/上下错误拼接，静止后不自愈。
2. **周期性抽动**：画面每秒左右抽风一次；滑动时狂闪。
3. **小元素花屏**：大面积滑动修好后，checkbox/按钮/扇形图等小元素交互时显示一坨花屏。
4. **小元素"不更新"**：花屏修好后，点 checkbox 显示还是旧样式（仔细看微微花），内部状态其实已切换；大规模重绘（滑动）后才显示正常。

每一层的根因完全不同，下面逐层讲。

---

## 2. 已确立的硬件事实（这些是排障的地基，全部经实测）

- **厂商 Arduino_GFX demo（`tmp/pio_music`，PCLK 16MHz）画面稳定** → 硬件/时序无恙。
- **IDF 5.5.5 官方 rgbtest 例程稳定** → 官方最小路径无恙。
- **PCLK 必须 16MHz**；12MHz 下面板不同步，退化成自检变色（黑红白绿蓝循环，约 1 秒一换）。如果看到全屏纯色循环，那是面板自检，说明 PCLK/时序没配上，不是程序在跑。
- **GFX 的 `Arduino_ESP32RGBPanel` 底层就是 `esp_lcd_new_rgb_panel`**（见 `tmp/pio_music/lib/Arduino_GFX-master/src/databus/Arduino_ESP32RGBPanel.cpp`）——和我们是同一个 IDF 驱动。所以"GFX 稳、我们抽"的差异在**用法**，不在驱动代码本身。
- CYD 2432S028R（SPI 屏）跑同一套 UI 无任何问题 → 问题在 RGB 并口链路。

## 3. 已排除项（逐项实测，别再浪费时间重试）

| # | 假设 | 结论 |
|---|------|------|
| 1 | WiFi 干扰 | CLI `wifioff` 彻底关驱动后仍抽 |
| 2 | XIP（代码/常量放 PSRAM） | 开也抽关也抽，最终决定不开（与 Arduino 对齐） |
| 3 | bounce buffer 10/20 行 | 两次都出随机细线+狂抽，弃用（`bounce_buffer_size_px=0`） |
| 4 | LVGL 全屏 PSRAM 缓冲 + PARTIAL | 更差，弃用 |
| 5 | LVGL DIRECT 直渲单 fb | 更闪（边扫边画撕裂可见），弃用 |
| 6 | smartdisplay 对齐配置（PLL160M / sram_trans_align=4 / psram_trans_align=64 / idle_low=0） | 无改善 |
| 7 | GFX 同款 flush 路径（SRAM 60 行缓冲 + 逐行 memcpy 直写 fb，不调 draw_bitmap/msync） | 仍抽——写屏路径与 GFX 完全等价，差异必在别处 |
| 8 | 应用代码 | 空 IDF 工程 + LVGL 9.3 + lv_demo_widgets 也抽 → 系统层问题 |
| 9 | CPU 频率 | 降 160MHz 仍抽；cache/PSRAM/flash 配置与 Arduino 工程 diff 一致 |

---

## 4. 第一层根因：欠载帧（抽动的物理学）

### 测量方法（这是整个排障的转折点——用数据代替猜测）

在 vsync ISR 里加统计（`esp_cpu_get_cycle_count()` 测相邻 vsync 间隔；GDMA EOF 回调置标志，vsync 到了 EOF 还没到 = 欠载帧 `dma_late`），每 5 秒打印：

```
vsync: 195 frames/5s (39.0 fps), period 25632..25640 us, dma_late=0    ← 静止
vsync: 195 frames/5s (39.0 fps), period 25632..25638 us, dma_late=56   ← 用户滑动中
```

### 实锤结论

1. **节拍无抖动**：vsync 间隔 25632~25640µs（<10µs 抖动），39.0fps 恒定——**不是时序问题**，别再动 PCLK/porch。
2. **`dma_late` 只在滑动时飙高**（56/46/89 每 5s）。机制：LVGL 大面积 memcpy 写 fb 挤爆 MSPI 总线 → EDMA 一帧拉不完 → LCD FIFO 见底 → 单帧扫出脏数据 → 肉眼"抽动"。
3. 佐证：widgets demo 三个分页里，整屏滑动的 Profile/Shop 页抽，只动小图表的 Analytics 页不抽（但有百叶窗=轻度撕裂）——**抽动幅度与每帧 memcpy 字节数正相关**。

## 5. 第二层根因：IDF 5.5.5 RGB 驱动把欠载的后果放大成永久故障

欠载帧偶发不可怕（人眼对单帧脏不敏感），可怕的是 IDF 5.5.5 驱动（`C:/esp/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c`）的两种模式都让欠载**留下后遗症**：

- **stream 模式（默认）**：`auto_next_frame=true`，LCD 控制器永不停，DMA 用环形链。欠载后 DMA 与 LCD 扫描的相位错位**永久保持** → 卷轴错位不自愈。
- **`CONFIG_LCD_RGB_RESTART_IN_VSYNC` 模式**：LCD 不停就在 vsync ISR 里重启 DMA，靠"restart_link 跳过 FIFO_PRESERVE 个像素"来猜 ISR 延迟。猜错就单帧错位 → 周期性抽动。驱动自己的注释都警告了这个风险。

### 对照：IDF 4.4 的模型（厂商 GFX demo 稳定的真正原因）

4.4 的 esp_lcd RGB 驱动（副本在 `C:\Users\m9291\AppData\Local\Temp\rgb_44.c`）：

- `auto_next_frame=false`：LCD 每扫完一帧**自动停下**；
- DMA 链一次性（末节点 `next=NULL`），发完即 EOF；
- VSYNC_END ISR 里做**全量重启**：`gdma_reset → lcd_ll_stop → fifo reset → gdma_start(链头) → 延迟 1µs → lcd_ll_start`。

因为 LCD 处于停止态，重启零竞态；**任何欠载/失步帧，下一帧必然自愈**。我们的 `rgb44.c` 就是用 5.5 的 HAL/GDMA API 复刻这套模型。

## 6. 第三层根因：LVGL DIRECT 双缓冲的换页耦合（小元素花屏）

换 4.4 模型后卷轴错位消失，但 DIRECT 双缓冲引入了新坑。要理解它，先要知道 LVGL 9.3 的三个内部机制（`managed_components/lvgl__lvgl/src/core/lv_refr.c`）：

1. **LVGL 自己做双缓冲同步**：`refr_sync_areas()` 在每帧刷新开头，把上一帧渲染过的区域从"在屏缓冲"拷到"离屏缓冲"，保证两块 fb 内容一致。
2. **flush_cb 的 `px_map` = `layer->draw_buf->data`**，即 `buf_act` 的基址（`lv_draw_buf_reshape` 只改 header 不动 data 指针）。
3. **一段刷新有多个脏区时会调多次 flush_cb，且 DIRECT 模式下 `buf_act` 只在最后一次 flush（`flushing_last`）后才交换**（`draw_buf_flush`，1376 行附近）。

由此推出 flush_cb 的两条铁律：

- **只在 `lv_display_flush_is_last(disp)` 为真的那次 flush 才请求换页**。中途换页 = 后续脏区渲进正在扫描的 fb = 花屏。典型触发场景：你点 checkbox 的同时 widgets demo 的动画元素也在动，一段刷新就有 2+ 个脏区。
- **请求换页后必须阻塞等换页在 vsync 真正生效，再 `lv_display_flush_ready()`**。否则 LVGL 的缓冲轮转（每帧交替）与物理扫描（vsync 才换页，最多晚 25.6ms）脱钩，LVGL 会往正在扫描的 fb 里渲染和做同步拷贝。rgb44 为此提供一个 swap 信号量，**仅在实际换页的那个 vsync 给出**，与 flush 严格 1:1。

锁死之后的不变量：LVGL 永远只渲染离屏 fb；每次渲染完的缓冲在下个 vsync 上屏；渲染速率被自然限到 39fps（与面板同频）。

## 7. 第四层根因：S3 的 GDMA 读 PSRAM 不过 cache（小元素"不更新"）

最诡异的一层：换页不变量经日志验证完全正确（渲哪块扫哪块），屏幕上却显示旧内容。

**机制**：ESP32-S3 的 GDMA 读 PSRAM 走 MSPI 自己的路径，**不 snoop D-cache 的脏行**。CPU（LVGL 直渲）写的新像素只躺在 D-cache 脏行里，EDMA 从 PSRAM 读到的是旧数据：

- **小元素更新**：只弄脏几~几十条 cache line，长期驻留不回写 → 屏幕一直显示旧样式（"点了没变"）；部分行碰巧被驱逐 → 新旧像素混杂（"微微花"）。
- **大滚动**：大面积写入制造驱逐压力，脏行被动快速回写 → 看着"正常"。
- 这解释了两个"未解之谜"：**GFX demo 为什么不 msync 也没事**——它每次 flush 都 memcpy 近百 KB（60 行 × 800px × 2B）到 fb，驱逐压力保证脏行几乎当帧回写；**IDF 官方驱动 direct 路径为什么每次 flush 都 msync 脏行**——它知道 GDMA 不过 cache。

**修复**：flush_cb 最后一次 flush、换页之前，对刚渲染的 fb 做回写：

```c
esp_cache_msync(px_map, LCD_H_RES * LCD_V_RES * 2,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
```

`DIR_C2M` 只回写脏行，代价上限 = cache 容量（几十 KB），不是真的搬 768KB。头文件是 `esp_cache.h`（esp_mm 组件），不是 `esp_cache_msync.h`。

---

## 8. 最终方案架构（rgb44 + LVGL 对接）

```
LVGL (core1)                rgb44 驱动                     硬件
─────────────              ──────────────────            ─────────────
渲染脏区 → fb[back]
最后 flush:
  msync(fb[back], C2M)  →  回写脏行到 PSRAM
  show_fb(px_map)       →  pending = back
  wait_swap(100ms)      →  阻塞 ──┐
                                  │   VSYNC_END ISR（每帧）:
                                  │     若 pending≥0: cur=pending,
                                  │       给 swap 信号量 ───────────┐
                                  │     gdma_reset                  │
                                  │     lcd_ll_stop                 │
                                  │     fifo_reset                  │
                                  │     gdma_start(link[cur])  ────→│→ DMA 新链
                                  │     delay 1µs                   │
                                  │     lcd_ll_start           ────→│→ LCD 重启扫描
  flush_ready ◄───────────────────┴─────────────────────────────────┘
  （LVGL 交换 buf_act，下一帧渲染另一块 fb=现在的离屏块）
```

要点清单：

- **双 fb**：PSRAM 各 768KB，64 字节对齐（`heap_caps_aligned_calloc`）。
- **每 fb 一条预建 GDMA 链**，换页只在 ISR 里换 `gdma_start` 的链头，零拷贝。节点容量按对齐后的 4032 字节算（`4095 & ~63`），否则 `gdma_link_mount_buffers` 报 "lli full need=191 avail=188"。
- `gdma_set_priority(chan, 3)` 提高 DMA 优先级。
- 换页信号量是二值信号量，**只在实际换页时给**——陈旧 token 会让 flush_cb 错过同步点。
- LVGL：`lv_display_set_buffers(disp, fb0, fb1, 800*480*2, LV_DISPLAY_RENDER_MODE_DIRECT)`。

### 5.5 私有 API 踩坑备忘（rgb44.c 里的编译坑，均已解决）

- 没有 `gpio_hal_iomux_func_sel`，用 `gpio_func_sel(gpio, PIN_FUNC_GPIO)`（`esp_private/gpio.h`）。
- `esp_clk_tree_enable_src` 在 `esp_private/esp_clk_tree_common.h`。
- `lcd_ll_enable_interrupt` 的宏包装要求作用域内有 `int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));`（它就是个变量名，不是宏）。
- `__DECLARE_RCC_ATOMIC_ENV` 配套的临界区用 `PERIPH_RCC_ATOMIC()`（`esp_private/periph_ctrl.h`）；esp_lcd 私有的 `LCD_CLOCK_SRC_ATOMIC` 用不了，自己 `#define` 等价宏。
- `XTHAL_GET_CCOUNT` 不可用，用 `esp_cpu_get_cycle_count()`（`esp_cpu.h`）。

---

## 9. 诊断工具箱（下次遇到显示问题直接用）

1. **ISR 统计**（`rgb44_stats`）：vsync 帧数/间隔 min~max/欠载次数，每 5s 打印。
   - 间隔稳 + late=0 + 画面抽 → 不是时序/总线问题，往换页/cache 方向查。
   - late 随滑动飙高 → 总线争抢欠载。
2. **flush 不变量检查**：最后一次 flush 时记录 `rgb44_cur()`，换页后再读一次——`cur` 必须切到刚渲染的块且发生变化。违例打印 `px_map/idx/cur 前后值/area`。
3. **面板自检识别**：全屏黑红白绿蓝循环 = 面板没收到正确时序在跑自检，查 PCLK。
4. **对照三件套**：厂商 GFX demo（`tmp/pio_music`，25 秒迭代）/ IDF 官方 rgbtest / 最小 LVGL 工程（`tmp/lvtest`）。

## 10. 排障原则（本项目用户明确要求）

- 不要拉 Arduino core 作依赖；GFX 已证明就是 esp_lcd，魔改库无意义。
- **每次实验只改一个变量**。
- 驱动级假设用 `tmp/pio_music`（25 秒迭代）或 `tmp/lvtest`（最小 IDF 工程）验证，不要直接在主工程上试。
- 用测量代替猜测：先加 ISR/计数器拿数据，再下结论。

## 11. 关键源码位置

| 内容 | 路径 |
|------|------|
| 本板 BSP（主工程） | `src/bsp/esp32/bsp_jc8048w550.c` |
| 自研 rgb44 驱动 | 主工程：`src/bsp/esp32/rgb44.c` / `rgb44.h`；实验原型：`tmp/lvtest/main/rgb44.c` |
| LVGL 对接参考（flush_cb 三条铁律） | `tmp/lvtest/main/main.c` |
| IDF 5.5.5 RGB 驱动（反面教材） | `C:/esp/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c` |
| IDF 4.4 RGB 驱动（正确模型） | `C:\Users\m9291\AppData\Local\Temp\rgb_44.c` |
| LVGL 刷新核心（sync/swap 语义） | `managed_components/lvgl__lvgl/src/core/lv_refr.c` |
| 厂商 GFX 对照工程 | `tmp/pio_music/src/lvgl_music_gt911_5.0.ino` |

## 12. 后记：滑动抽动的第二轮排查——帧中 EDMA 饥饿（流量墙）

字库换成 GB2312 全表后用户报告滑动抽动。这一轮排查推翻了好几个直觉结论，按时间顺序：

### 12.1 测量先行：面板侧完全健康

`lcdstat` CLI（`bsp_lcd_stats_print`，渲染/换页分离计时）实测滑动窗口：

- vsync 周期 25630~25641µs（抖动 <10µs）、`dma_late≈0` —— 面板扫描零欠载；
- swap max = 恰好一个帧周期 —— 换页从不 miss vsync，无卷轴错位；
- **work（纯渲染+msync）avg=87.7ms/帧**（-O2 后），swap 9.6fps —— 滑动时每帧都是全屏脏区。

### 12.2 被推翻的三个假设

1. **"refr=33 与 39fps 错拍是根因"** —— 改成 `CONFIG_LV_DEF_REFR_PERIOD=15` 后无改善。
   该改动保留（消除无意义错拍），但它不是抽动的根因。
2. **"-O2 能救"** —— `CONFIG_COMPILER_OPTIMIZATION_PERF=y` 后 work 从 ~110ms 降到 87.7ms，
   抽动依旧。**渲染瓶颈不是 CPU 算力，是 MSPI 总线流量。**
3. **"大字号文字 RLE 压缩解压太贵"** —— 本板 UI_FONT_BIG=1，正文全走 28/32 号不压缩字体，
   14/16 的 RLE 根本不在热路径上。为此生成的双变体（`font_cjk_14_cmp.c` / `font_cjk_16_cmp.c`，
   CYD 分支用压缩版省 flash）对本板行为零影响。

### 12.3 当前主导假设：帧中 EDMA 饥饿

DIRECT 模式一次全屏渲染的 MSPI 流量：渲染写 fb 768KB + `refr_sync_areas()` 同步拷贝
1.5MB + flush 前 `esp_cache_msync` 整帧回写 768KB + EDMA 扫描读 768KB ≈ **4.3MB/帧**。
PSRAM 可用带宽实测等效只有 ~49MB/s，而 EDMA 扫描本身就需要 30MB/s（800×480×2B×39fps）
**持续不断**的份额。CPU 侧的渲染/回写是突发式大块流量，瞬间挤占总线 → EDMA 在**帧中间**
FIFO 见底 → 当前帧某行扫错 → 下一帧自愈。这就是"抽动"：所有计数器（vsync 抖动、
dma_late、swap miss）都看不见帧内单行错误，只有肉眼可见。

佐证：FULL 模式实验（砍 1.5MB 同步拷贝）work 降到 69ms，滑动抽动减轻，但静止时
1Hz 时钟每秒触发整帧渲染+回写爆发 → **每秒自抽一次**，更糟，已回退 DIRECT。

已排除的其他方向：GDMA 配置与 IDF 驱动逐项对比一致（burst 64B、
`eof_till_data_popped=false`，我们还多设了 priority 3）；`CONFIG_ESP32S3_DATA_CACHE_LINE_64B`
实测静止自抽 + 滑动花屏加剧，已回退 32B（64B 让 cache 驱逐粒度翻倍，回写突发更大）。

### 12.4 最终定位与修复：全表字库的 flash 流量（用户实测确认）

**最小字库实验一锤定音**：给 JC8048 生成 `_min` 最小子集字体（仅 UI 源码字面量，
几百字；28 号 671KB / 32 号 850KB，对比全表 15MB/19MB），`ui_layout.c` 里加
`UI_FONT_MIN` 编译开关让该板链 `_min` 版——**滑动抽动完全消失，静止也干净**。

机制：6840 字的 GB2312 全表字形数据放在 flash（.rodata），LVGL 渲染文本时经
XIP cache 读取。表越大，字形地址越分散，cache 局部性越差 → 滑动列表每帧重绘
数百个字形时，cache miss 引发的 **flash 突发读取** 在 MSPI 总线上与 PSRAM 的
EDMA 扫描/渲染流量三方争抢 → 帧中 EDMA 饥饿 → 抽动。字库缩到 1/20 后，
字形几乎全命中 cache，flash 流量消失，总线只剩 PSRAM 一家的事，EDMA 不再挨饿。

这也解释了为什么"第一次成功的散点固件"不抽：当时字库就是小子集；换成全表后
才开始抽。字库大小从一开始就是变量，只是被混进了显示链路排障里。

当前配置（JC8048 出货状态）：

- `UI_FONT_MIN=1`：最小子集字体。代价：文件名/SSID 里表外汉字显示方框。
  要回全表置 0 即可（`tools/fontgen/gen_fonts.py` 两种都会生成）；
- DIRECT 双缓冲 + 阻塞 flush（第 6/7 节三条铁律不变）；
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`（-O2）、`CONFIG_LV_DEF_REFR_PERIOD=15`、
  data cache line 32B 保留。

### 12.5 若以后要养活全表字库，候选杠杆（按优先级）

1. PSRAM 120MHz 实验性提速（`CONFIG_SPIRAM_SPEED_120M` + `CONFIG_IDF_EXPERIMENTAL_FEATURES`，
   +50% 带宽；S3R8 官方只标 80MHz，有稳定性风险）；
2. 中间档字库：GB2312 一级字 3755 字（约为全表一半），或常用 1000~2500 字表，
   在覆盖面和 cache 局部性之间折中——`_min` 已证明方向正确，逐级加大表可找到
   不抽的最大容量；
3. msync 按脏行范围回写（BSP 记 cur∪prev 脏行位图，小更新不用整帧 768KB）；
4. vsync 量化（渲染超帧就锁每 2/3 个 vsync 换页，只治节奏不治 glitch）；
5. 终极：bounce buffer（EDMA 从内部 SRAM 行缓冲读，免疫 PSRAM/flash 争抢）——
   rgb44 暂无此路径，工作量大。

**教训**：这套链路上"面板侧计数器全绿"不代表"没问题"——帧内 EDMA 饥饿不留下任何
计数器痕迹。而且**渲染内容的存储位置**（flash vs PSRAM vs 内部 RAM）本身就是
总线流量变量，排显示问题别忘了把字库/图片资源也算进流量账。排查顺序：先测节拍，
再测掉帧，最后算流量墙——流量账要算全（PSRAM 渲染 + 回写 + EDMA + flash XIP）。
