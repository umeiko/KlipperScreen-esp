# 板型移植模板

所有新板型都从这个目录开始。现有 BSP 含有各自硬件的特殊处理，只适合查阅，
不应整份复制后修改。

1. 先阅读 [`docs/porting.zh.md`](../../docs/porting.zh.md) 的“查清硬件”和“单独点亮屏幕”章节，确认屏幕是 SPI、I80、RGB、QSPI 还是 MIPI。
2. 把 `bsp_board_template.c` 复制为 `src/bsp/esp32/bsp_<board>.c`。
3. 把 `sdkconfig.defaults.board_template` 复制为
   `src/ports/esp32/sdkconfig.defaults.<board>`。
4. 使用 CST816S 电容触摸时，再复制 `touch_input_board_template.c/.h`，文件名和
   所有 `board_template` 都替换为板型名；其它触摸控制器把它作为适配层示例。
5. 替换所有 `BOARD_TEMPLATE`、`board_template` 和 `TODO(board)` 标记。
6. 按 [`docs/contributing-board.zh.md`](../../docs/contributing-board.zh.md) 的清单登记板型。
7. 先构建新板型，再构建一个现有板型检查共享代码。

C 模板是最小的 **SPI 命令屏 + ST7789** 示例。只有同类 SPI 屏才能在它上面逐项修改。
I80、RGB/DOTCLK、QSPI 和 MIPI 必须替换整个显示 transport，包括总线、缓冲和
`flush_cb`；只改 `esp_lcd_new_panel_st7789()` 不会把 SPI 模板变成并口实现。
公共 BSP 生命周期仍然可以保留。没有触摸的板不要创建假的 pointer。

输入按硬件三选一：触摸、触摸 + 旋钮、纯旋钮。只有 XPT2046 一类电阻触摸需要
`touch.json` 和校准流程；GT911、CST816S 一类电容触摸通常直接报告屏幕坐标，
不调用校准函数。`touch_input_board_template.c` 是 CST816S 的完整实现示例，包含
中断后读 I2C、坐标上报和息屏唤醒。CST816T 等相近型号仍需先核对数据手册，不能
只看名称就假设与 CST816S 驱动兼容。

旋转编码器属于共享的可选输入，不要把 EC11/编码器驱动塞进板型 BSP。只需在板型
sdkconfig defaults 中启用 `Optional input devices`，填写 A、B、按键三个 GPIO；
它会自动与触摸输入并存；纯旋钮板也使用同一配置，UI 不需要板型分支。
