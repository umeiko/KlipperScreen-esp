# virtual-printer —— 本地虚拟 Klipper 打印机

调试 KlipperScreen-esp 时无需真实打印机：在本机（或局域网任意 Linux 主机）
跑一个**假的 klippy** + **真的官方主线 Moonraker**，对局域网伪装成一台
正常工作的 3D 打印机。

## 原理

```
KlipperScreen-esp ──WebSocket/HTTP──> Moonraker（官方主线，全真）
                                          │ klippy_uds_address
                                          ▼
                                    fake_klippy.py（Python 模拟器）
```

- App 只和 Moonraker 通信，Moonraker 是真的 → API 行为 100% 真实
  （文件管理/元数据/缩略图/history/授权/job_queue 全走真代码路径）。
- `fake_klippy.py` 实现 klippy 的 webhooks 协议（`\x03` 分隔的 JSON-RPC，
  见 `klippy/webhooks.py`）：`info` / `objects/list|query|subscribe` /
  `gcode/script|help|restart|firmware_restart` / `gcode/subscribe_output` /
  `pause_resume/*` / `query_endstops/status` / `emergency_stop`，
  以及 4Hz 差速订阅推送（`process_status_update`）与 gcode 控制台输出。
- 打印机形态由 `runtime/printer_data/config/printer.cfg` 决定：
  改配置（加风扇/传感器/宏/轴数……）重启 fake_klippy 即生效。
  热模型一阶惯性 + 噪声；打印走带按字节速率回放真实 gcode 文件
  （温度/移动/M73/层信息/exclude_object 全部真实执行）。

## 快速开始

```bash
# Linux 主机：bash 直接跑
scripts/virtual-printer/install.sh
scripts/virtual-printer/run.sh

# Windows 本机：经 MSYS2 运行（tools/msys64）
tools/msys64/usr/bin/bash.exe -lc "cd <仓库目录> && scripts/virtual-printer/install.sh"
tools/msys64/usr/bin/bash.exe -lc "cd <仓库目录> && scripts/virtual-printer/run.sh"
```

启动后 Moonraker 监听 `0.0.0.0:7125`：

- 桌面 App：设置里填 `127.0.0.1:7125`（或本机 LAN IP）。
- ESP32：填运行机器的 LAN IP（Windows 首次会弹防火墙放行 python）。

管理命令：`run.sh` / `stop.sh` / `status.sh`。

## 目录

```
scripts/virtual-printer/
  fake_klippy.py            # 模拟器本体（纯标准库）
  install.sh / run.sh / stop.sh / status.sh
  config/printer.cfg        # 虚拟打印机定义（模板）
  config/moonraker.conf     # Moonraker 配置（模板）
  patches/                  # 打在官方主线上的 patch（install 时自动应用）
    0001-klippy-tcp-transport.patch          # klippy 传输支持 tcp://（无 UDS 的平台用；全平台应用）
    0002-metadata-pil-optional.patch         # 无 Pillow 时缩略图仍可提取 PNG（无 Pillow 才应用）
    0003-file-manager-inotify-optional.patch # inotify_simple 导入可选（仅 Windows/MSYS2 应用）
    0004-none-observer-metadata-scan.patch   # observer=none 时启动也扫描 gcode 元数据（全平台应用）
  tools/make_sample_gcode.py          # 生成带缩略图的示例 gcode
  tests/smoke_test.py                 # 端到端冒烟（模拟 App 握手+打印生命周期）
  repos/                  # gitignored：官方 moonraker / klipper 浅克隆
  runtime/                # gitignored：printer_data（config/gcodes/logs/comms/database）+ venv
```

## 常用调试技巧

- **调打印速度**：`FAKE_KLIPPY_BPS=4096 bash run.sh`（默认 16384 B/s，
  `demo_cube.gcode` 约 17 秒；`quick_line.gcode` 约 6 秒）。
- **改打印机形态**：编辑 `runtime/printer_data/config/printer.cfg` 后
  `stop.sh && run.sh`。想恢复模板用 `install.sh --force-config`。
- **升级官方主线**：`install.sh --update`（`git pull` 后 patch 自动重放；
  patch 冲突说明上游变了，需人工跟进）。
- **模拟持久化**：探针 z_offset / 网床 profile 存在
  `runtime/printer_data/logs/fake_klippy_saved.json`，删掉即"恢复出厂"。
- **看协议细节**：`runtime/printer_data/logs/klippy.log`、
  `moonraker.log`、以及 stdout 日志。

## 冒烟测试

```bash
python3 scripts/virtual-printer/tests/smoke_test.py [host] [7125]
```

覆盖：握手链 → 对象订阅 → 升温 → 归位/移动 → 限位 → 推送 →
控制台回显 → 文件列表/缩略图 → 打印开始/暂停/恢复/取消/完成 →
history → 急停 → firmware_restart 恢复。

## 与真实打印机的差异（已知取舍）

- klippy 侧为模拟：gcode 只实现常见命令；未知命令会按 klippy 惯例报错。
- 无真实动力学：移动瞬时完成（G28 有 ~1s 模拟耗时）；温度是一阶模型。
- 宏体不支持 Jinja，只支持 `{params.X|default(..)}` 占位。
- Windows 下 klippy 传输用 `tcp://127.0.0.1:7126`（MSYS2 的 asyncio
  不支持 AF_UNIX 客户端），Linux 下用标准 unix socket。
- `update_manager` 未启用（App 的更新面板只在 Linux 上位机/Android 显示；
  需要时可自行在 moonraker.conf 中添加，repos/klipper 是真实克隆）。
