# 贡献新板型（PR 指南）

给仓库贡献一块新板子的支持，需要“一个 BSP 实现 + 全链路登记”。必须从 [`templates/board/`](https://github.com/umeiko/KlipperScreen-esp/tree/main/templates/board) 开始；现有 BSP 只用于查阅某个控制器的特殊实现，不作为复制底板。开始动手前先读[移植指南](porting.zh.md)。

## 命名约定

板型标识用**小写下划线**的型号名（如 `cyd_2432s028r`、`e32r35t`），Kconfig 宏为 `CONFIG_BOARD_<大写>`。构建产物、CI 矩阵、sdkconfig 文件名全部使用同一个标识。

## 需要修改/新增的文件清单

| # | 文件 | 改动 | 必需？ |
|---|---|---|---|
| 1 | `src/bsp/esp32/bsp_<board>.c` | 复制板型模板，只填写标出的硬件区块；整文件包在 `#if CONFIG_BOARD_<BOARD>` 里 | ✅ |
| 2 | `src/bsp/Kconfig.projbuild` | `choice BOARD` 里加 `config BOARD_<BOARD>` | ✅ |
| 3 | `src/bsp/CMakeLists.txt` | SRCS 加新文件；新驱动芯片时 REQUIRES 加组件名 | ✅ |
| 4 | `src/ports/esp32/entry/idf_component.yml` | 新驱动芯片时加 managed component 依赖及版本约束 | 视情况 |
| 5 | `src/ports/esp32/sdkconfig.defaults.<board>` | 新建：`CONFIG_BOARD_<BOARD>=y` + flash 模式/大小/主频（照抄同芯片板型改宏即可） | ✅ |
| 6 | `tools/build-esp32.sh` | `board_conf()` 加 case（TARGET/BDIR/SDKCFG/DEFS），`all` 分支加构建，头部注释更新 | ✅ |
| 7 | `.github/workflows/build.yml` | `firmware` 矩阵加一项（board/target/bdir/sdkcfg/defs/chip/flash_size/bl_offset）；`package` 矩阵加一项 | ✅ |
| 8 | `src/ui/ui_layout.c` | 字号档加 `#elif defined(CONFIG_BOARD_<BOARD>)` | ✅ |
| 9 | `README.md` / `README_zh.md` | 支持板型列表加一行（含状态标注） | ✅ |
| 10 | `docs/boards.md` / `docs/boards.zh.md` | 硬件信息与引脚表（即本站"支持的板子"页） | ✅ |
| 11 | `src/ports/esp32/partitions_<board>.csv` | 仅当 flash 布局与默认 `partitions.csv` 不同时 | 视情况 |
| 12 | `src/ports/esp32/sdkconfig.<board>` | 首次构建生成的完整 sdkconfig，建议提交（与 jc8048w550 先例一致），避免 CI 重新生成时漂移 | 建议 |

## 规则与注意事项

- **不要按板型裁剪编译**：`src/bsp/CMakeLists.txt` 里所有板的源文件都参与编译，靠文件内 `#if CONFIG_BOARD_*` 裁剪。原因是组件注册早于 kconfig 加载，`if(CONFIG_...)` 在 CMake 层面拿不到。
- **新板型一律标 WIP**：CI `package` 矩阵里 `suffix: "-WIP"`，README 和 boards 页同步标注。真机验证稳定后再去标。
- **WIP 说明文案**：workflow 里的 WIP.txt 步骤按 `matrix.board` 自动生成，无需单独改。
- **Bootloader 偏移**：ESP32/ESP32-S3 用 `bl_offset: 0x1000`；C3 等新启动链的芯片是 `0x0`。
- **README 双语同步**：`README.md` 与 `README_zh.md` 都要改。

## 提交前的验证

1. 本地全量构建新板型通过：`bash tools/build-esp32.sh <board>`
2. **回归构建**至少一块既有板型（共享文件有改动时必须）：`bash tools/build-esp32.sh cyd_2432s028r`
3. 固件大小检查：构建末尾的 `binary size` 不超过应用分区（当前 0x320000）
4. 按移植教程先做不带 LVGL 的五色色带测试，再验证开机动画、完整 UI 和声明的全部输入
5. 没有真机就在 PR 里明确注明“未经真机验证”，板型继续保持 WIP

## PR 里请附上

- 板子资料链接（厂商 wiki / 原理图 / 引脚表）
- 屏幕接口分类（SPI/I80/RGB/QSPI/MIPI）、控制器型号、稳定频率或时序参数
- 五色色带和完整 UI 的真机照片或串口日志；没有则明确说明
- 与模板 BSP 的有意差异点（例如共总线、无 RST、强制校准），方便 review
- 输入形态：触摸、旋钮、两者都有或都没有。旋钮 GPIO 写进 Kconfig/sdkconfig，不写私有 UI 实现

提交信息风格参考历史：`feat: 新增 <板型> 板型支持（<屏幕> + <触摸/旋钮>，WIP）`。
