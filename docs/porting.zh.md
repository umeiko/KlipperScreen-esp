# 移植到自己的开发板

本项目把"板级差异"收敛到 **BSP（板级支持包）** 一层：UI、Moonraker 协议、网络等业务代码完全不感知硬件。移植 = 为你的板子写一个 BSP 实现文件 + 几处清单式登记。

## 1. BSP 接口契约

所有板型（含桌面模拟器）实现同一组接口，定义在 [`src/bsp/bsp.h`](https://github.com/umeiko/KlipperScreen-esp/blob/main/src/bsp/bsp.h)：

| 函数 | 语义 |
|---|---|
| `bsp_init()` | 板级初始化：显示 + 触摸 + LVGL 节拍任务。`app_main` 第一个调用 |
| `bsp_get_display()` | 返回 `lv_display_get_default()` |
| `bsp_lvgl_lock()` / `bsp_lvgl_unlock()` | LVGL 线程互斥（递归锁），所有 LVGL API 必须持锁调用 |
| `bsp_restart()` | 重启设备（先把"重启中"toast 画出来再 `esp_restart`） |
| `bsp_lcd_push(x,y,w,h,px)` | 直接把一块 RGB565 像素推上屏（开机动画等 LVGL 场景外渲染用） |
| `bsp_delay_ms(ms)` | 毫秒延时 |
| `bsp_set_brightness(0-100)` | 背光亮度（ESP32 走 LEDC PWM） |
| `bsp_fade_out(ms)` | 背光渐变到纯黑，阻塞至完成（语言切换重启前调用） |
| `bsp_set_screen_timeout(sec)` | 自动息屏超时，0=永不；超时关背光，触摸唤醒 |
| `bsp_time_sync_from_host(host,port)` | **不用自己实现**：`bsp_wifi_esp32.c` 已提供板型无关实现 |

`bsp_conf.h`（`bsp_conf_read/write` 配置持久化）由共用文件 `bsp_conf_littlefs.c` 实现，也无需动。

## 2. 实现步骤

### 2.1 选一个最近的邻居做模板

| 你的板子类型 | 复制哪个 BSP |
|---|---|
| ESP32 + SPI 屏 + XPT2046 电阻触摸 | `bsp_cyd_2432s028r.c`（触摸独立总线）或 `bsp_e32r35t.c`（触摸与屏共总线） |
| ESP32-S3 + RGB 并口屏 + I2C 电容触摸 | `bsp_jc8048w550.c` + `rgb44.c` |

新建 `src/bsp/esp32/bsp_<board>.c`，整文件包在 `#if CONFIG_BOARD_<BOARD>` 里（板型宏由 Kconfig 生成）。

### 2.2 BSP 内必须处理的事

1. **引脚定义**：写在文件顶部宏里（本项目约定引脚不进 sdkconfig）。
2. **显示驱动**：优先用 Espressif 组件仓库的 managed component（已用：`esp_lcd_ili9341`、`esp_lcd_st7796`、`esp_lcd_touch_xpt2046`、`esp_lcd_touch_gt911`）。新芯片要在 `src/ports/esp32/entry/idf_component.yml` 加依赖、`src/bsp/CMakeLists.txt` 的 REQUIRES 加组件名。
3. **LVGL 缓冲模型**：
   - SPI 屏：内部 RAM 双缓冲（`MALLOC_CAP_DMA`，几十行高）+ `LV_DISPLAY_RENDER_MODE_PARTIAL`
   - RGB 并口屏：PSRAM 双帧缓冲 + `LV_DISPLAY_RENDER_MODE_DIRECT` + vsync 换页（参考 rgb44）
4. **flush_cb 字节序**：SPI 屏要求高字节在前，LVGL 内存是小端 RGB565 → flush 时就地交换字节（**不要**用 `LV_COLOR_FORMAT_RGB565_SWAPPED`，实测雪花屏）。
5. **DMA 异步**：`esp_lcd_panel_draw_bitmap` 是异步的，复用/释放像素缓冲前必须等 `on_color_trans_done`（见现有 BSP 的信号量用法）。
6. **触摸坐标**：XPT2046 取原始 12bit ADC（`x_max/y_max = 4096`），不做驱动层镜像，用两点线性校准映射到屏幕坐标并持久化到 LittleFS（`touch.json`）；GT911 这类电容屏直接用驱动输出的坐标。
7. **背光与息屏**：LEDC PWM + `screen_off_check` 周期检查（照抄模板）。

### 2.3 登记板型（构建链路）

按[贡献指南](contributing-board.md)的清单登记 Kconfig / CMakeLists / sdkconfig.defaults / 构建脚本。本地构建：

```bash
bash tools/build-esp32.sh <board>          # 构建（首次自动 set-target）
bash tools/build-esp32.sh <board> flash COM6   # 构建 + 烧录
bash tools/build-esp32.sh <board> menuconfig   # 改配置
```

!!! warning "sdkconfig 大坑"
    改 `sdkconfig.defaults.<board>` 对**已生成**的 `sdkconfig.<board>` 不生效——要改必须两个文件都改；注意 sdkconfig 里 `# CONFIG_XXX is not set` 会覆盖 defaults。

### 2.4 UI 适配

- 逻辑分辨率由 `lv_display_create(w, h)` 决定，UI 按 `scr_h / 240` 自动缩放，无需改面板代码。
- `src/ui/ui_layout.c` 里按板型定死字号档（预处理期裁剪，省 flash）：小屏（320×240/480×320）`UI_FONT_BIG 0`，大屏（800×480）`1`。
- 触摸不灵/坐标乱：先跑两点校准（删除 LittleFS 里的 `touch.json` 重启即可重新校准），再考虑改驱动层 swap/mirror。

## 3. 桌面模拟器

UI 代码可以在电脑上直接调（不用刷机）：

```bash
bash tools/build-desktop.sh
```

产物是 SDL2 窗口程序，`KLIPPER_RES=800x480` 环境变量可切分辨率。开发面板布局、截图验证都用它，BSP 层在 `src/ports/desktop/` 有空操作实现。
