# 移植到自己的开发板（零基础教程）

这篇教程假设你**几乎没碰过 ESP32 开发**，目标是：手把手带你把本项目的固件移植到一块新板子上，并且本地编译出可以烧录的产物。全程以 **CYD 2432S028R**（2.8 寸电阻屏，最经典最便宜的板子）的 BSP 文件 `src/bsp/esp32/bsp_cyd_2432s028r.c` 为参考实现，一个函数一个函数地讲。

整个过程分五步：

1. 准备开发环境（装 ESP-IDF）
2. 拉代码、先编译一块已有板子验证环境
3. 查清你的板子的硬件信息
4. 写 BSP 文件（本教程的主体，逐函数讲解）
5. 登记板型 + 编译出产物 + 排错

---

## 第 0 步：准备环境

### 0.1 安装 ESP-IDF v5.5.5

ESP-IDF 是乐鑫官方的开发框架，**版本必须是 5.5.5**（本项目锁定此版本）。

- **Windows**：下载 [ESP-IDF 在线安装器](https://dl.espressif.com/dl/esp-idf/)，选择 v5.5.5 安装。装完后你应该有：
    - IDF 源码：`C:\esp\v5.5.5\esp-idf`
    - 工具链：`C:\Espressif\tools`
- **Linux/macOS**：按 [官方文档](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.5/esp32/get-started/) 用 `install.sh` 安装。

本项目自带环境包装脚本 `tools/idf.ps1` / `tools/idf-env.bat`，构建脚本会自动调用它们激活环境，你**不需要**手动配置 PATH。

### 0.2 拉代码

```bash
git clone https://github.com/umeiko/KlipperScreen-esp.git
cd KlipperScreen-esp
```

### 0.3 先编译一块已有板子，验证环境没问题

```bash
bash tools/build-esp32.sh cyd_2432s028r
```

第一次构建要 3~10 分钟（要联网拉依赖组件）。看到末尾这样的输出就说明环境 OK：

```
Project build complete. To flash, run:
 idf.py flash
...
klipper_remote_display.bin binary size 0x2d55a0 bytes. Smallest app partition is 0x320000 bytes. ...
```

产物在 `src/ports/esp32/build/` 下：`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`klipper_remote_display.bin`。

如果你手上正好有一块 CYD，可以顺手烧录验证整条链路：

```bash
bash tools/build-esp32.sh cyd_2432s028r flash COM6   # COM6 换成你的串口号
```

!!! tip "串口号怎么查"
    Windows 设备管理器 → 端口（COM 和 LPT）；Linux 一般是 `/dev/ttyUSB0`。

---

## 第 1 步：查清你的板子的硬件信息

动手写代码前，必须从**厂商 wiki / 原理图**里确认下面这些信息（缺一个都别开工）：

| 要查的项 | 例子（CYD） | 去哪查 |
|---|---|---|
| 主控型号 | ESP32（双核 240MHz） | 板子丝印 / 商品页 |
| Flash 大小 | 4MB | 商品页 / `esptool flash_id` |
| 屏幕驱动 IC | ILI9341 | 厂商 wiki / 排线丝印 |
| 屏幕接口 | SPI | 同上 |
| 屏幕分辨率 | 240×320（竖屏原生） | 同上 |
| 触摸 IC | XPT2046（电阻） | 同上 |
| 触摸接口 | SPI（独立总线还是和屏共用？） | 引脚表 |
| 全部引脚 | SCLK=14, MOSI=13, ... | 厂商引脚分配表 |
| 背光引脚 + 点亮电平 | GPIO21，高电平点亮 | 原理图 |

常见查找渠道：厂商 wiki（如 lcdwiki）、原理图 PDF、别人写好的 TFT_eSPI `User_Setup.h`（里面就是现成的引脚定义）。

!!! warning "SPI 屏特别注意"
    触摸屏的 SPI 是和屏幕**共用一组引脚**还是**独立一组**，决定了 BSP 里要初始化几条 SPI 总线。CYD 是独立的（共享实测 MISO 无应答），E32R35T 是共用的——两种写法本项目里都有现成例子。

---

## 第 2 步：写 BSP 文件（逐函数教程）

### 2.0 先理解什么是 BSP

本项目所有"和硬件有关"的代码都隔离在一层叫 BSP（Board Support Package，板级支持包）的东西里。上层 UI 只调用 `bsp_xxx()` 函数，完全不关心你是什么屏、什么触摸。

所以移植 = 写**一个 C 文件**，实现 `src/bsp/bsp.h` 里声明的函数。打开 `src/bsp/bsp.h` 可以看到全部 11 个函数，这就是我们接下来要逐个实现的清单。

### 2.1 创建文件骨架

复制参考实现再改是最快的：

```bash
cp src/bsp/esp32/bsp_cyd_2432s028r.c src/bsp/esp32/bsp_myboard.c
```

文件的第一行和最后一行是**板型开关**，必须改：

```c
#include "sdkconfig.h"
#if CONFIG_BOARD_MYBOARD        // ← 改成你的板型宏

// ... 全部实现 ...

#endif /* CONFIG_BOARD_MYBOARD */
```

**为什么要有这个开关**：构建系统会把所有板子的 BSP 文件**全部编译**（CMake 组件注册早于 Kconfig 加载，没法在 CMake 层面按板型过滤源文件），所以每个文件自己用 `#if` 包住，只有选中的板型才会编译出实际内容。宏名规则：`CONFIG_BOARD_` + 板型名大写。

### 2.2 引脚定义

文件顶部的宏，照你第 1 步查到的引脚表填。CYD 的样子：

```c
#define PIN_LCD_SCLK   14    // SPI 时钟
#define PIN_LCD_MOSI   13    // SPI 主机输出
#define PIN_LCD_MISO   12    // SPI 主机输入
#define PIN_LCD_CS     15    // 屏幕片选
#define PIN_LCD_DC     2     // 数据/命令切换
#define PIN_LCD_RST    4     // 屏幕复位（没有就填 -1）
#define PIN_LCD_BL     21    // 背光

#define PIN_TP_SCLK    25    // 触摸 SPI（独立总线时才有这三个）
#define PIN_TP_MOSI    32
#define PIN_TP_MISO    39
#define PIN_TOUCH_CS   33    // 触摸片选
#define PIN_TOUCH_IRQ  36    // 触摸中断

#define LCD_H_RES      320   // 横屏逻辑宽度
#define LCD_V_RES      240   // 横屏逻辑高度
#define LCD_SPI_HZ     (40 * 1000 * 1000)  // 屏幕 SPI 速度
#define DRAW_BUF_LINES 40    // LVGL 绘图缓冲行数
```

要点：

- `LCD_H_RES/V_RES` 填**横屏**后的逻辑分辨率（CYD 原生竖屏 240×320，横屏就是 320×240）。
- `DRAW_BUF_LINES` 是 LVGL 每次渲染多少行再送屏。缓冲大小 = `H_RES × 行数 × 2 字节`，CYD 是 320×40×2 = 25KB，要开两块。ESP32 内部 RAM 紧张，别贪大，20~40 行合适。
- RST 如果和 ESP32 的 EN 共用了（如 E32R35T），填 `-1`，驱动会自动改用软件复位。

### 2.3 `bsp_lvgl_lock` / `bsp_lvgl_unlock` —— LVGL 线程锁

**干什么**：LVGL 不是线程安全的。本项目 LVGL 跑在独立任务里，其他任务（网络回调、串口 CLI）要动 UI 前必须先拿锁。

```c
static SemaphoreHandle_t lvgl_mux;

void bsp_lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
void bsp_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }
```

**解释**：就是一个 FreeRTOS 递归互斥锁的两个包装函数。`lvgl_mux` 在 `bsp_init()` 开头创建。**照抄，不用改任何字。**

### 2.4 `bsp_get_display` / `bsp_delay_ms` / `bsp_restart` —— 三个一行流

```c
lv_display_t *bsp_get_display(void) { return lv_display_get_default(); }

void bsp_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void bsp_restart(void)
{
    lv_refr_now(NULL);                 // 先把"重启中"的提示画上屏
    vTaskDelay(pdMS_TO_TICKS(800));    // 给用户 800ms 看到它
    esp_restart();
}
```

**解释**：

- `bsp_get_display`：返回 LVGL 默认显示器（在 `bsp_init` 里创建的那个）。上层拿它查分辨率。
- `bsp_delay_ms`：开机动画用的普通延时。
- `bsp_restart`：语言切换要重建全部 UI，实现方式是直接重启。先 `lv_refr_now` 强制刷新一帧，不然提示画不上去就重启了。

**全部照抄。**

### 2.5 `bsp_set_brightness` —— 背光亮度

**干什么**：设置页里的亮度滑条调它。ESP32 上用 LEDC（硬件 PWM）控制背光引脚。

```c
static uint8_t bl_duty = 255;
static int     bl_pct = 100;

void bsp_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0 && pct < 5) pct = 5;     // 兜底：防止设成 1% 黑屏后摸不到设置
    bl_pct = pct;
    bl_duty = (uint8_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, bl_duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

**解释**：把 0~100 的百分比换算成 8bit PWM 占空比（0~255）写进 LEDC 通道。LEDC 定时器和通道的初始化在 `bsp_init` 里做（第 2.11 节会讲）。**照抄**；如果你的板子背光是**低电平点亮**（少见），把占空比反过来写 `255 - bl_duty`。

### 2.6 自动息屏三件套 —— `bsp_set_screen_timeout` + 两个内部函数

**干什么**：设置页可以设"N 秒无操作自动息屏"，触摸任意位置唤醒。

```c
static uint32_t so_after_s;      // 超时秒数，0 = 永不
static bool     screen_off;
static int64_t  last_act_us;

void bsp_set_screen_timeout(uint32_t sec)
{
    so_after_s = sec;
    last_act_us = esp_timer_get_time();
    if (screen_off) {                    // 正在息屏时改了设置 → 先唤醒
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_activity(void)        // 触摸回调里调用：打点 + 唤醒
{
    last_act_us = esp_timer_get_time();
    if (screen_off) {
        screen_off = false;
        bsp_set_brightness(bl_pct);
    }
}

static void screen_off_check(void)       // LVGL 任务里周期调用
{
    if (screen_off || !so_after_s) return;
    if (esp_timer_get_time() - last_act_us > (int64_t)so_after_s * 1000000) {
        screen_off = true;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);   // 息屏 = 背光 PWM 归零
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}
```

**解释**：一个状态机。`last_act_us` 记录最后一次触摸时间，`screen_off_check` 每 5ms 检查一次有没有超时。息屏只关背光（LCD 内容还在），所以唤醒是即时的。**全部照抄。**

### 2.7 `bsp_fade_out` —— 优雅关机式淡出

**干什么**：切语言重启前，背光用硬件渐变平滑变暗，然后把屏幕整屏推黑（不然面板 GRAM 里残留旧帧，下次上电会闪一下旧画面）。

```c
void bsp_fade_out(uint32_t ms)
{
    static bool fade_installed;
    if (!fade_installed) {
        ledc_fade_func_install(0);       // LEDC 渐变功能要装一次中断服务
        fade_installed = true;
    }
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, ms);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    bl_duty = 0;

    static uint16_t black[LCD_H_RES * 40];    // 静态数组零初始化 = 全黑
    for (int y = 0; y < LCD_V_RES; y += 40) {
        xSemaphoreTake(lcd_trans_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 40, black);
        xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));
    }
}
```

**解释**：前半段是 LEDC 硬件渐变；后半段每次推 40 行黑色块直到整屏。`lcd_trans_done` 信号量下一节解释。**照抄**（行数 40 与 `DRAW_BUF_LINES` 无关，只是凑块大小，可以不用改）。

### 2.8 `on_color_trans_done` + `bsp_lcd_push` —— DMA 推屏

**干什么**：`bsp_lcd_push` 是"把一块 RGB565 像素直接推上屏"的底层函数，开机动画用它（那时 LVGL 还没跑起来）。

**先要理解一个坑**：`esp_lcd_panel_draw_bitmap()` 是 **DMA 异步**的——函数返回时数据还在往外搬。如果你立刻释放或改写像素缓冲，DMA 就会读到垃圾，画面出现条状撕裂。所以必须注册"传输完成"回调，用一个信号量等它：

```c
static SemaphoreHandle_t lcd_trans_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    LV_UNUSED(io); LV_UNUSED(edata); LV_UNUSED(user_ctx);
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(lcd_trans_done, &hp);   // DMA 完成，在中断里给信号量
    return hp == pdTRUE;
}

void bsp_lcd_push(int x, int y, int w, int h, const uint16_t *px)
{
    /* SPI 屏要求先发像素高字节，LVGL/内存里是小端 → 拷一份交换字节再推 */
    size_t n = (size_t)w * h;
    uint16_t *tmp = malloc(n * 2);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) tmp[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    xSemaphoreTake(lcd_trans_done, 0);            // 清掉残留信号
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, tmp);
    xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(500));  // 等 DMA 搬完
    free(tmp);
}
```

**解释**：

- 字节序交换：RGB565 在内存里低字节在前，SPI 屏要求高字节在前。这里交换是因为 `px` 是调用方的缓冲不能改，所以拷一份。
- `xSemaphoreTake(lcd_trans_done, 0)`（超时 0）的作用是"清空"，防止上次遗留的信号造成误判。
- 回调的注册在 `bsp_init` 里。

**照抄**；如果你的屏是 RGB 并口（不用 SPI），不需要字节交换，参考 `bsp_jc8048w550.c`。

### 2.9 `flush_cb` —— LVGL 与屏幕之间的桥（全文件最重要）

**干什么**：LVGL 每渲染完一块区域就调它，你的任务是**把这块像素送上屏**。

```c
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 就地交换字节序（不要用 LV_COLOR_FORMAT_RGB565_SWAPPED，实测雪花屏） */
    uint16_t *p = (uint16_t *)px_map;
    int32_t n = (int32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    for (int32_t i = 0; i < n; i++) p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);    // 告诉 LVGL"这块缓冲你可以再用了"
}
```

**解释**：

- `area` 是脏区域的坐标，`px_map` 是像素数据。
- 注意这里**就地**改 `px_map`（LVGL 的缓冲反正下一轮要重画），而 `bsp_lcd_push` 是拷贝——区别在于调用方身份不同。
- `lv_display_flush_ready` 必须调，且 LVGL 双缓冲机制保证这次 flush 的 DMA 读完之前不会给你同一个缓冲（配合 `on_color_trans_done` 由 esp_lcd 内部处理）。

**照抄。**（SPI 屏通用。RGB 并口屏走 DIRECT 模式，完全不同，参考 rgb44。）

### 2.10 触摸部分 —— `tp_read_raw` / 校准 / `touch_read_cb`

**干什么**：把触摸芯片的原始读数变成屏幕坐标。电阻屏（XPT2046）因为安装方向、走线差异，每台都得校准，所以本项目用**两点线性校准**，参数存 LittleFS 的 `touch.json`。

核心思路：**驱动层不做任何坐标变换**，直接拿 12bit 原始 ADC 值（0~4095），校准公式 `screen = raw × 斜率 + 截距` 会吸收所有镜像/交换。

这一整块（`tp_read_raw`、`touch_cal_save`、`touch_cal_load`、`cal_pump`、`cal_sample`、`touch_cal_run`）大约 160 行，**建议原样照抄**，只需要理解两件事：

1. `tp_read_raw` 里交换了 x/y：

    ```c
    *x = pt[0].y;   /* CYD 横屏安装下 raw 轴与屏幕轴交叉，交换后
        *y = pt[0].x;      *x 恒为屏幕水平（长）轴原始值 */
    ```

    你的板子轴怎么交叉**不用管**——两点校准的斜率带符号，任何方向都能吸收。照抄即可。

2. `touch_cal_load()` 的行为决定**首次启动要不要进校准**：
    - CYD 有真机测出的出厂默认值 → 文件缺失时写默认值、不进校准。
    - 你的新板子没有出厂值 → 让 load 在文件缺失时 `return false`，`bsp_init` 里就会自动进阻塞式校准流程（照抄 `bsp_e32r35t.c` 的写法，把 `touch_cal_load` 末尾的 `return true` 改成 `return false`）。

`touch_read_cb` 是 LVGL 的触摸读取回调，除了校准映射外还有两个小机关：

```c
} else if (pressing && esp_timer_get_time() - last_valid_us < 50 * 1000) {
    rx = last_rx;   /* 滑动中采样瞬时丢失 → 桥接为仍按住，防止手势被拆成点按 */
    ry = last_ry;
}
```

以及息屏唤醒那次点击会被"吞掉"（`wake_swallow`），防止唤醒屏幕的同时误触按钮。**照抄。**

!!! note "电容屏（GT911 等）"
    不用校准，驱动直接输出屏幕坐标，`touch_read_cb` 简单得多。参考 `bsp_jc8048w550.c`。

### 2.11 `bsp_init` —— 初始化总装（最长的函数，拆成 7 小段）

这是上电后第一个跑的函数。对照 `bsp_cyd_2432s028r.c` 的 401~532 行，按顺序讲每一段：

**① 锁 + NVS + LittleFS**

```c
lvgl_mux = xSemaphoreCreateRecursiveMutex();

esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
}

esp_vfs_littlefs_conf_t fs_conf = {
    .base_path = "/littlefs",
    .partition_label = "storage",
    .format_if_mount_failed = true,
    .dont_mount = false,
};
ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));
```

创建 2.3 节的互斥锁；NVS 是 WiFi 模块要用的键值存储；LittleFS 挂载 `storage` 分区（分区表 `partitions.csv` 里定义好了）到 `/littlefs`，用来存 `touch.json` 校准文件，首次启动自动格式化。**照抄。**

**② 背光 LEDC 初始化**

```c
ledc_timer_config_t bl_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 5000,
    .clk_cfg = LEDC_AUTO_CLK,
};
ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
ledc_channel_config_t bl_ch = {
    .gpio_num = PIN_LCD_BL,                 // ← 你的背光引脚
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 255,                            // 上电全亮
    .hpoint = 0,
};
ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));
```

**只有引脚要改。** 8bit / 5kHz 是通用值。

**③ SPI 总线初始化（LCD）**

```c
spi_bus_config_t buscfg = {
    .sclk_io_num = PIN_LCD_SCLK,
    .mosi_io_num = PIN_LCD_MOSI,
    .miso_io_num = PIN_LCD_MISO,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = LCD_H_RES * DRAW_BUF_LINES * 2 + 8,  // 一次 DMA 最大搬运量
};
ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
```

**改引脚。** `max_transfer_sz` 必须 ≥ 一块 LVGL 缓冲的字节数，按公式照抄。

**④ 触摸 SPI 总线（仅独立总线的板子需要）**

```c
spi_bus_config_t tp_buscfg = { ... PIN_TP_SCLK ... };
ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &tp_buscfg, SPI_DMA_DISABLED));
```

触摸是低速设备，不用 DMA。**如果你的板子触摸与屏共用总线，删掉这段**（参考 `bsp_e32r35t.c`），第 ⑥ 步里把触摸挂到 `SPI2_HOST`。

**⑤ 屏幕：IO 句柄 + 驱动 + 方向**

```c
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_panel_io_spi_config_t io_cfg = {
    .dc_gpio_num = PIN_LCD_DC,
    .cs_gpio_num = PIN_LCD_CS,
    .pclk_hz = LCD_SPI_HZ,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
    .spi_mode = 0,
    .trans_queue_depth = 10,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

/* 传输完成信号量：bsp_lcd_push / fade_out 等 DMA 用 */
lcd_trans_done = xSemaphoreCreateBinary();
esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_color_trans_done };
ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL));

esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = PIN_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // 颜色偏红蓝对调就改成 RGB
    .bits_per_pixel = 16,
};
ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle));  // ← 换你的驱动
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
/* 横屏：swap_xy 转 90°，mirror 调镜像方向 */
ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
```

这段是**你最可能要改的地方**：

- 驱动函数换成你的屏幕 IC 对应的：`esp_lcd_new_panel_ili9341` / `esp_lcd_new_panel_st7796` / …（组件仓库里有几十种，搜 [components.espressif.com](https://components.espressif.com)）。换驱动还要登记依赖，见第 3 步。
- ST7796 类屏幕通常还要加一句 `esp_lcd_panel_invert_color(panel_handle, true)`（不然颜色像底片）。
- `swap_xy(true)` + `mirror(true, true)` 决定横屏方向。画面左右/上下颠倒就调 mirror 的两个参数（排列组合 4 种，试一次只要重新编译烧录）。
- `rgb_ele_order`：颜色红蓝互换就 BGR ↔ RGB 切换。

**⑥ 触摸初始化**

```c
esp_lcd_panel_io_handle_t tp_io;
esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_TOUCH_CS);
ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &tp_io_cfg, &tp_io));
//                                            共总线的板子这里写 SPI2_HOST ↑

esp_lcd_touch_config_t tp_cfg = {
    .x_max = 4096,   // 拿原始 12bit ADC，不做驱动层换算（交给两点校准）
    .y_max = 4096,
    .rst_gpio_num = GPIO_NUM_NC,
    .int_gpio_num = PIN_TOUCH_IRQ,
    .levels = {.reset = 0, .interrupt = 0},
    .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
};
ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &touch_handle));
```

**改总线号和引脚。** `x_max/y_max = 4096` + 三个 flags 全 false 是刻意的：原始值进校准。

**⑦ LVGL 初始化 + 启动任务**

```c
lv_init();
lv_tick_set_cb(tick_cb);          // 告诉 LVGL 怎么读毫秒时钟

lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
size_t buf_sz = LCD_H_RES * DRAW_BUF_LINES * 2;
void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);   // DMA 能读内部 RAM
void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_flush_cb(disp, flush_cb);                 // 挂上 2.9 的桥

lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, touch_read_cb);              // 挂上 2.10 的触摸

/* 无校准数据 → 进两点校准（LVGL 任务没起，校准函数内部自己泵帧） */
if (!touch_cal_load()) {
    touch_cal_run();
}

xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);
```

要点：双缓冲（一块在 DMA 送屏、一块给 LVGL 渲染）；`MALLOC_CAP_DMA` 必须加（SPI DMA 读不了 PSRAM 和普通堆）；`lvgl_task` 就是每 5ms 跑一次 `lv_timer_handler()` + `screen_off_check()` 的循环（照抄）。

---

到这里 BSP 文件就写完了。`bsp_time_sync_from_host` 和配置持久化（`bsp_conf_*`）是**全板共用**的实现，不用你写。

## 第 3 步：登记板型（5 个文件，每个加几行）

| 文件 | 干什么 | 加什么 |
|---|---|---|
| `src/bsp/Kconfig.projbuild` | 让 `CONFIG_BOARD_MYBOARD` 宏存在 | `choice BOARD` 里加 `config BOARD_MYBOARD` + 一行描述 |
| `src/bsp/CMakeLists.txt` | 让新 .c 参与编译 | SRCS 里加 `"esp32/bsp_myboard.c"`；换了新驱动 IC 时 REQUIRES 加组件名 |
| `src/ports/esp32/entry/idf_component.yml` | 声明第三方驱动依赖 | 换了新驱动 IC 时加一行，如 `espressif/esp_lcd_st7796: "^1.4.0"` |
| `src/ports/esp32/sdkconfig.defaults.myboard` | 新板型的默认配置 | 新建，照抄同芯片板子的，把 `CONFIG_BOARD_XXX=y` 换成你的宏 |
| `tools/build-esp32.sh` | 让构建脚本认识新板子 | `board_conf()` 里加一个 case：`TARGET=esp32; BDIR=build-myboard; SDKCFG=sdkconfig.myboard; DEFS="sdkconfig.defaults;sdkconfig.defaults.myboard"` |

sdkconfig.defaults 的完整内容（ESP32 经典款照这个抄）：

```
CONFIG_BOARD_MYBOARD=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y    # 你的 flash 不是 4MB 就改
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
```

!!! warning "sdkconfig 大坑"
    构建过一次后会生成完整的 `sdkconfig.myboard`，之后再改 `sdkconfig.defaults.myboard` **不生效**——要改必须两个文件都改（sdkconfig 里搜同名配置行，注意 `# CONFIG_XXX is not set` 会覆盖 defaults）。

另外 `src/ui/ui_layout.c` 里加一个字号档（决定用大字还是小字，预处理期裁剪省 flash）：

```c
#elif defined(CONFIG_BOARD_MYBOARD)
#define UI_FONT_BIG 0    // 320×240 / 480×320 用 0；800×480 用 1
```

## 第 4 步：编译、烧录、验证

```bash
bash tools/build-esp32.sh myboard              # 首次自动 set-target + 全量编译
bash tools/build-esp32.sh myboard flash COM6   # 编译 + 烧录
```

**怎么算成功**：构建末尾打印 `Project build complete`，且 `binary size` 没超过 `Smallest app partition`。烧录后屏幕亮起、出现开机动画 → 主界面，就大功告成。

串口日志（115200 8N1）能看到 `BSP ready (...)` 和触摸校准加载情况，有问题先看日志。

## 第 5 步：常见问题对照表

| 现象 | 原因 | 改哪里 |
|---|---|---|
| 全白/全黑，背光亮 | 驱动 IC 不对 / SPI 模式不对 | `esp_lcd_new_panel_xxx` 换驱动 |
| 颜色像底片（反色） | 面板需要 INVON | 加 `esp_lcd_panel_invert_color(panel, true)` 或改 false |
| 红蓝互换 | RGB/BGR 序错 | `rgb_ele_order` BGR ↔ RGB |
| 画面颠倒/镜像 | 安装方向不同 | `esp_lcd_panel_mirror` 两个参数排列组合 |
| 颜色雪花/噪点 | 误用 `RGB565_SWAPPED` 格式 | 用回就地字节交换的 flush_cb |
| 触摸完全没反应 | 总线接错 / 共总线 vs 独立总线搞错 | 核对第 1 步的引脚表 |
| 触摸位置乱 | 没校准 | 删 `touch.json` 重启进校准（串口 CLI `rm /littlefs/touch.json`） |
| 画面条状撕裂 | DMA 缓冲被提前复用 | 检查 `on_color_trans_done` 信号量逻辑 |
| 编译报 `esp_lcd_new_panel_xxx` 未声明 | 依赖没登记 | 第 3 步的 idf_component.yml + CMakeLists |

确认稳定后想把板子贡献回仓库？看[贡献新板型（PR 指南）](contributing-board.md)。
