# 界面截图

逐个面板的功能展示。全部截图取自 Windows 桌面版（800×480），连接的是仓库自带的[虚拟打印机](https://github.com/umeiko/KlipperScreen-esp/tree/main/scripts/virtual-printer)（假 klippy + 真实主线 Moonraker），所以界面上的温度、风扇、宏、文件列表、打印进度都是真实 API 数据而非摆拍。所有面板与 ESP32 固件共享同一套 UI 代码，小屏机型使用相同布局的紧凑主题。

## 主界面与打印

### 主界面

![主界面——空闲](screenshots/panels/main.png)

顶部状态卡一眼看清连接状态（绿=已连接，黄=断连，红=Klipper 异常），卡上显示打印机名、地址，打印时滚动显示进度与文件名。中部六宫格进入温度/移动/菜单/文件/打印/设置；底部是急停（M112）与重启下位机（FIRMWARE_RESTART），均需二次确认。打印中点状态卡可直达打印状态页。

### 打印状态

![打印状态——打印中，带 G-code 缩略图](screenshots/panels/job_status.png)

当前任务的实时视图：切片缩略图与进度百分比（无缩略图能力的平台显示进度圆环）、文件名与层数（当前/总层）、已用与剩余时间、喷嘴和热床的当前/目标温度（点温度卡可跳温度页）。底部按钮随任务状态变化：打印中=暂停/取消/急停，暂停时变「继续」，任务结束后变「重启（重打）+主菜单」。标题栏齿轮进入微调页。

### 微调

![微调——Z 偏移 / 速度 / 流量](screenshots/panels/fine_tune.png)

打印中/暂停时的运行时微调：Z 偏移按 0.01/0.05mm 步进（点数值归零），速度（M220）与流量（M221）按 1/5/10/25% 可选步进调整。所有改动只影响运行时，不写 SAVE_CONFIG；任务结束后本页自动退出。仅 Klipper 模式且打印中/暂停时可进入。

### 打印文件

![打印文件——分页文件列表](screenshots/panels/files.png)

G-code 文件列表，每页 8 行，带上一页/刷新/下一页，按路径字典序排列（排序由 Moonraker 完成，忽略大小写）。文件名超长滚动显示，大小以 KB/MB 计。点某行进入详情页（打印/删除，删除为两步确认）。ESP32 上列表走流式解析，文件再多内存占用也不涨。

## Klipper 控制（菜单页进入）

### 菜单

![菜单](screenshots/panels/menu.png)

Klipper 专属工具入口：挤出、传感器、宏、控制台、风扇、Z 校准。

### 温度控制

![温度控制——预设与目标温度](screenshots/panels/temperature.png)

喷嘴、热床两张大卡显示当前温度（0.1° 精度）与目标温度；底部预设行一键 PLA / PETG / ABS / 全部冷却。目标温度可点按弹数字键盘输入；带旋转编码器的机型可按下进入就地编辑，旋转调值（快转自动从 1°C 加速到 10°C 步进），停手 300ms 自动提交。拓竹云端监视模式下为只读。

### 移动

![移动——点动与归位](screenshots/panels/move.png)

X/Y/Z 三轴按 0.1/1/10/50mm 步进点动，底部 XY/Z/全部归位三键，标题栏右侧「关闭电机」（M84，需确认）。未归位的轴位置显示为 `?`。

### 挤出

![挤出](screenshots/panels/extrude.png)

挤出量档位（5/10/25/50mm）+ 挤出/回抽两个大按钮。页面实时显示喷嘴温度与最低挤出温度，低于 170°C 拒绝挤出并 toast 提示。

### 风扇

![风扇](screenshots/panels/fan.png)

逐行列出打印机全部风扇：可控风扇（模型风扇、`fan_generic`）用 0~100% 滑条调节，**松手才**下发 M106/SET_FAN_SPEED；温控风扇（`heater_fan`、`controller_fan`）为只读进度条。

### 传感器状态

![传感器——限位与断料检测](screenshots/panels/sensors.png)

X/Y/Z 限位开关状态（触发/未触发/未知），右上角手动刷新并显示「更新于 N 秒前」；下方列出断料传感器，实时显示有料/无料，并可开关检测。

### 宏

![宏](screenshots/panels/macros.png)

列出打印机配置里的全部 G-code 宏，点击即执行。桌面端每行还有直接执行按钮；带参数的宏会多出编辑按钮，先进入参数表单（默认值从 `{params.X|default(...)}` 预填）再执行。

### 控制台

![控制台](screenshots/panels/console.png)

G-code 终端：回显滚动区按类型着色（本机命令/错误/警告，M105 温度轮询自动过滤），底部命令输入带虚拟键盘，另有清空按钮。历史来自 `server.gcode_store`，实时行来自 `notify_gcode_response` 推送。

### Z 校准

![Z 校准](screenshots/panels/zcalibrate.png)

对标 KlipperScreen 的首层校准流程：选择 PROBE_CALIBRATE 或 Z_ENDSTOP_CALIBRATE，用 TESTZ 以六档步距（0.01~5mm）抬降喷嘴，「接受」保存校准，「中止」（需确认）放弃。校准活跃状态由 `manual_probe.is_active` 订阅驱动。

## 设置

### 设置

![设置](screenshots/panels/settings.png)

无线网络、打印机连接、语言、显示设置与关于页（含版本号）的入口。Wi-Fi 行仅在有 Wi-Fi 能力的平台出现。

### 打印机连接设置

![打印机连接设置](screenshots/panels/moonraker.png)

按槽位编辑连接：机器模式、切换打印机、打印机名称、主机、端口（默认 7125）、API Key（掩码显示）。旋钮机型的主机字段有专用 IPv4 逐段编辑器；触摸/键盘则弹文本框。「保存并连接」落盘后立即重连；状态行实时显示连接状态与应用层心跳延迟（ms）。

### 切换打印机

![切换打印机——6 槽位](screenshots/panels/printers.png)

2×3 网格共 6 个打印机槽位，每槽显示模式 Logo（Klipper/拓竹）、名称与地址，当前槽整卡高亮。点击即切换活动连接。

### 机器模式

![机器模式](screenshots/panels/machine_mode.png)

在 Klipper（Moonraker）与拓竹两种模式间切换当前槽位，切换需确认；模式决定可用的面板与能力范围。

### 显示设置

![显示设置](screenshots/panels/display.png)

显示相关开关集中于此：反色/旋转 180°/水平镜像（硬件支持的屏）、屏幕方向（0/90/180/270°，桌面端）、背光与自动息屏（ESP32 与 Linux 上位机）、编码器步进（带 20 秒转动测试确认，编码器机型）、屏幕色序、深色/浅色主题。

### 背光

![背光](screenshots/panels/brightness.png)

大号百分比 + 滑杆实时调光（5%~100%，下限 5% 防止拉到全黑），**松手才**写入配置，避免拖动过程刷 flash。

### 无线网络

![无线网络](screenshots/panels/wifi.png)

顶部状态卡（已连接 SSID + IP），下方 AP 扫描列表（加密/开放标识 + 信号强度图标），点加密 AP 弹密码输入，连接中有转圈进度层与超时兜底。凭据落盘，开机自动回连。支持 ESP32、Windows（netsh）、Linux（nmcli）。

### 语言

![语言](screenshots/panels/language.png)

界面语言列表，当前语言高亮。ESP32 保存后重启生效；桌面端免重启，原地重建整个 UI。

### 关于

![关于](screenshots/panels/about.png)

版本号、板型/构建目标、框架版本（ESP-IDF / SDL2、LVGL）、仓库地址与协议。Linux 上位机与 Android 端另有「检查更新」入口。

## 拓竹

### 连接方式

![拓竹连接方式](screenshots/panels/bambu_link.png)

选择拓竹槽位的连接链路：云端监视（状态/温度/进度，只读）或局域网控制（开发者模式）。

### 拓竹连接

![拓竹连接](screenshots/panels/bambu_setup.png)

Bambu 云账号登录与绑机：中国区/全球区切换（中国区支持手机号 + 短信验证码，邮箱则密码登录），支持 TFA 双重验证流程；登录后列出账号下全部打印机（名称、型号、在线状态），点击绑定为当前槽位设备。
