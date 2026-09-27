# Bambu Cloud Monitor：ESP32 实施架构与 Kimi 任务单

> 制定日期：2026-09-13
> 适用仓库：`umeiko/KlipperScreen-esp`
> 目标：以可审查、可回退的小提交，让 ESP32 获得与 Windows 产品版相同的 Bambu Cloud 只读监视闭环。
>
> 进度更新（2026-09-23）：提示词 1–4 的主体已落地；CYD 中国区短信登录、
> MQTT/TLS 连接和温度状态读取已真机验证。剩余工作以第 5 节压力测试清单为准，
> 实测故障与修法记录在 `src/ports/esp32/bambu/DEVNOTES.md`。

## 1. 本轮产品边界

本轮只交付 **Bambu Cloud Monitor**：

- 中国区 / 全球区登录；
- 密码、邮件验证码、中国区短信验证码和 TFA；
- 获取账号绑定的设备列表并选择当前打印机；
- MQTT over TLS 实时读取状态、任务名、温度、进度、剩余时间和层数；
- 凭据过期、网络中断、无打印机等状态有明确反馈；
- ESP32 与 Windows 共用现有 UI 和公共打印机读模型。

本轮明确不做：

- Bambu LAN Developer Mode；
- 灯光、暂停、继续、取消、温控、运动、打印文件等任何控制；
- AMS 完整 UI、摄像头、FTPS、多打印机后台常驻；
- `printer_model.c` 的全量 store/backend-vtable 重构；
- 固件版本号、tag 和 release。

Cloud Monitor 的能力位继续为 `0`。这不是遗漏，而是防止任何按钮把操作误发给 Moonraker。

## 2. 为什么首版改为设备上直接登录

旧草案倾向“Windows 登录后把 token 配对给 ESP32”。那条路线最终更理想，但现在还需要另做发现、一次性配对认证和安全传输协议，工作量与风险都大于云监视本身。

首版复用已有 `panel_bambu_setup.c`，在 ESP32 上直接完成登录。约束如下：

- 网页/API 细节只允许存在于 `src/ports/esp32/bambu/`，不能进入 UI、`printer_model.c` 或公共头文件；
- 密码、验证码和 TFA 会话只在当前操作的 RAM 中存在，用完立即清零；
- NVS 只保存 `region + account + user_id + access_token`，每个打印机槽仍只保存非秘密的序列号、名称和型号；
- 默认固件没有启用 flash encryption，因此 NVS 是“独立秘密存储”，不是“加密保险箱”。文档必须如实说明物理读取 flash 仍可能取得 token；
- 未来改为桌面配对时，只替换凭据入口，不改 MQTT、状态解析和 UI 读模型。

## 3. 目标结构

```text
现有公共层（保留）
  panel_bambu_setup.c
      ↓ 只调用 bambu_cloud.h
  bambu_cloud.h / bambu_monitor.h
      ↓ 快照接口
  printer_model.c
      ↓ 只读 getter，capabilities=0
  panels

ESP32 端口（新增）
  src/ports/esp32/bambu/
    bambu_cloud_esp32.c       公共 cloud API 的薄封装
    bambu_monitor_esp32.c     公共 monitor API 的薄封装
    bambu_runtime_esp32.c/.h  唯一网络 actor、快照、生命周期
    bambu_http_esp32.c/.h     有界 HTTPS 请求与 Cookie/CSRF
    bambu_secret_esp32.c/.h   单一账号 profile 的 NVS 存储

公共、无平台状态解析（新增）
  src/core/bambu_status_stream.c/.h
```

Windows 的 `bambu_cloud_winhttp.c` 和 `bambu_monitor_openssl.c` 保持原样。不要为了“去重”先重构一份已经能工作的实现。

### 3.1 单一网络 actor

ESP32 端只有一个长期存在的 `bambu_net` FreeRTOS actor。UI API 只更新 desired state 或向小队列投递命令，绝不直接做 DNS、TLS、HTTP、MQTT stop/destroy。

actor 串行执行：

```text
HTTP 登录/刷新到来
  → 停止并销毁 MQTT
  → 确认 MQTT/TLS 资源释放
  → 执行一次 HTTPS 操作
  → 清理 HTTP client、响应体和秘密临时副本
  → 若仍为已登录且存在 desired serial，再启动 MQTT
```

这样保证无 PSRAM 的 CYD 上不会同时常驻两套 TLS 会话。`bambu_cloud_*` 与 `bambu_monitor_*` 虽然是两组公共 API，底层也不允许各自创建一套互不知情的任务。

队列里传“堆对象指针”，不要放四份含 2 KiB token/password 的大结构。入队失败、取消、任务完成三个出口都必须清零并释放对象。监视目标序列号属于 desired state，用锁保护，不必反复分配。

### 3.2 后端生命周期

当前 `printer_model.c` 在 Bambu 模式会停 Bambu monitor，却不会停已经启动的 Moonraker。正式接入前必须补齐非阻塞 `moonraker_stop()`：

```text
Klipper → Bambu:  stop Moonraker → start/maintain Bambu
Bambu → Klipper:  stop Bambu    → start/maintain Moonraker
```

网络对象的实际销毁不能发生在 LVGL 回调里。ESP32 使用 timer/worker 上下文，Windows/POSIX 唤醒已有 worker 并关闭活动 socket。停止后允许工作任务本身继续休眠存在，但不得保留 socket、TLS client、重连计时或活动请求。

### 3.3 MQTT 与大包

- Broker：中国区 `cn.mqtt.bambulab.com:8883`，全球区 `us.mqtt.bambulab.com:8883`；
- MQTT 3.1.1；用户名 `u_{user_id}`；密码为 access token；
- 订阅 `device/{serial}/report`，发布到 `device/{serial}/request`；
- TLS 使用 `esp_crt_bundle_attach` 并校验 broker hostname；严禁 insecure 或跳过 CN；
- ESP-MQTT RX buffer 先用 4096 B、TX buffer 1024 B，依靠 `total_data_len/current_data_offset` 接收分片；
- 单条 report 上限 49152 B；超限或分片 offset 不连续时丢弃整条消息，不得解析半包；
- 首次有效连接发送一次 `pushall`；快速重连不得高频发送，同一目标最短间隔 5 分钟；
- QoS 0，keepalive 60 秒，使用库的 PING；不要再造一套裸 MQTT 协议。

Bambu 完整状态可能超过 30 KiB。CYD 禁止“48 KiB 原文 + cJSON DOM”双份占堆。`MQTT_EVENT_DATA` 必须边收边喂给定长、零堆分配的 `bambu_status_stream`，只保留需要的标量和任务名。桌面端原有 cJSON 解析器暂不更换。

### 3.4 流式状态解析器

建议 API：

```c
void bambu_status_stream_begin(bambu_status_stream_t *p, size_t total_len);
bool bambu_status_stream_feed(bambu_status_stream_t *p,
                              const char *chunk, size_t len);
bool bambu_status_stream_finish(bambu_status_stream_t *p,
                                bambu_status_t *in_out);
```

要求：

- parser context 定长，内部不得 `malloc/realloc/cJSON_Parse`；
- 只读取根对象下 `print` 对象的直接子字段；其他层级里的同名字段必须忽略；
- 支持任意字节位置分片、JSON 转义、UTF-8、负数/小数/指数、嵌套未知对象和数组；
- 识别现有 `bambu_status.c` 的全部字段；
- 缺字段保持旧值；完整且合法的消息结束时才原子合并；
- 畸形、截断、超限消息不得部分污染旧快照。

### 3.5 HTTP 与秘密

ESP 端照现有 Windows 流程实现，不自行猜新接口：

- API host：`api.bambulab.cn` / `api.bambulab.com`；
- Site host：`bambulab.cn` / `bambulab.com`；
- 密码与验证码登录：`/v1/user-service/user/login`；
- 邮件验证码：`/v1/user-service/user/sendemail/code`；
- 中国区短信验证码：`/v1/user-service/user/sendsmscode`；
- TFA：先 `/api/csrf`，再 `/api/sign-in/tfa`；
- 设备列表：`/api/v1/iot-service/api/user/bind`；
- 非 JWT 短 token 的 UID fallback：`/v1/user-service/my/profile`。

HTTP 响应按块增长，默认接收块 2048 B，总上限 65536 B。有 PSRAM 时响应体优先放 PSRAM；无 PSRAM 时使用 internal 8-bit heap。每次请求结束必须 cleanup client 并释放响应。不得打印 body、Authorization、Cookie、CSRF、token、密码、验证码或完整 UID。

NVS namespace 使用不超过 15 字符的固定名，例如 `kr_bambu`。profile 带 schema version，读取时任一必需字段缺失/过长/损坏即整体视为 signed out。logout 用 `nvs_erase_all + nvs_commit`，同时清零 RAM 副本。

## 4. 内存与并发硬约束

- 不允许网络回调直接调用 LVGL；UI 只轮询加锁快照；
- MQTT event handler 不做阻塞 stop/destroy，不调用 HTTP；
- cloud snapshot 和 monitor snapshot 分别用静态 mutex 保护；禁止持锁做网络、JSON 解析或 NVS I/O；
- token 最大 2048 B，任何复制前先检查长度；
- 任务栈先从 actor 6144 B、ESP-MQTT 6144 B 起步，用 `uxTaskGetStackHighWaterMark` 实测后再调整；
- 所有临时秘密统一走一个显式清零 helper，清零操作不得被编译器优化掉；
- 不修改 `managed_components/`；
- 不因为一次内存失败就打开 insecure TLS、降低证书校验或把大 buffer 改成静态常驻；
- sdkconfig 只有在测量证明需要时才改；一旦改，必须同时修改 defaults 和所有已经生成的对应 sdkconfig canonical 行。

## 5. 验收门槛

构建门槛：

```bash
bash tools/build-desktop.sh
bash tools/build-esp32.sh all build
```

桌面回归：Windows Bambu 登录、设备列表和实时状态保持原行为；Klipper 连接与控制不受影响。

ESP 真机至少覆盖：

- 一台无 PSRAM ESP32（优先 `cyd_2432s028r`）；
- 一台有 PSRAM ESP32-S3（优先 `esp32s3-JLC-SZP`）；
- 中国区真实账号；若有全球区账号再补全球区，不能假装已经验证；
- 密码或验证码登录至少一条，TFA 只有真实账号要求时才算已验证；
- 连接 30 分钟，状态、温度、进度持续变化；
- Klipper/Bambu 来回切换 20 次；Bambu 页面进入退出 20 次；Wi-Fi 断开恢复 5 次；
- 登录前后、MQTT 首包后、稳定 10 分钟后分别记录 `mem`、`taskmem`；有疑似增长再跑 `ht`。

无 PSRAM板稳态最低要求：当前 free heap 不持续下降，10 分钟斜率绝对值小于约 256 B/min；free heap 建议保持至少 35 KiB，largest block 至少 20 KiB。若硬件基线达不到建议值，必须报告实测值和最大分配需求，不能把“能连一次”写成完成。

## 6. 给 Kimi 的执行方式

Kimi 适合做边界清楚的单模块移植和按日志修正，不适合一次承担“重构核心 + 登录 + MQTT + 内存优化 + UI”。下面提示词必须 **一次只发一条**。每条完成后先看它的 diff、构建输出和真机证据，再发下一条。

每个阶段只允许一个主题提交。验收不通过就留在该阶段修，不得用“先把后面也做了”掩盖问题。

### 提示词 1：先修后端生命周期

```text
请先完整阅读仓库根目录 AGENTS.md 和 docs/bambu-esp32-kimi-handoff.md，只做“提示词 1”，不要开始 ESP Bambu 登录或 MQTT。

目标：补齐 Moonraker 的可停止生命周期，保证 Klipper 与 Bambu 后端不会同时持有网络连接。

具体要求：
1. 在 src/core/moonraker_client.h 增加非阻塞、幂等的 moonraker_stop()。
2. 为 ESP32、Windows WinHTTP、POSIX 和 stub 四个实现补齐该函数。
3. stop 只能表达 desired disabled 并唤醒各自的 worker/timer；socket/WebSocket 的阻塞关闭与 destroy 不得在 LVGL 调用栈执行。
4. stop 后取消/抑制重连、heartbeat 和待处理 RPC；moonraker_send_rpc/moonraker_rpc 在 disabled 时返回 false。
5. 再次 moonraker_start() 必须能恢复连接，不能永久杀死 worker。
6. printer_model.c 在 Bambu 模式幂等 stop Moonraker，在 Klipper 模式 stop Bambu monitor。不要做 backend-vtable 重构。
7. 不改 Bambu stubs，不改 UI，不改协议和 sdkconfig。

验收：运行 bash tools/build-desktop.sh 和 bash tools/build-esp32.sh all build。检查 git diff，不得含无关格式化。说明各平台 stop 如何唤醒阻塞读，并列出可能仍存在的竞态。验收通过后单独提交，提交信息：core: separate Moonraker and Bambu lifecycles
```

### 提示词 2：零分配流式状态解析器

```text
请阅读 AGENTS.md 和 docs/bambu-esp32-kimi-handoff.md，只做“提示词 2”。不要写 HTTP、MQTT、NVS 或 UI。

目标：新增一个平台无关、定长、零堆分配的 Bambu report 流式解析器，供 ESP-MQTT 分片直接喂入；Windows 现有 bambu_status.c 行为保持不变。

要求：
1. 新增 src/core/bambu_status_stream.c/.h，API 和语义按计划文档 3.4。
2. 禁止 malloc/calloc/realloc/free 和 cJSON；parser context 必须定长。
3. 只接受根对象 print 的直接子字段；支持任意分片、转义字符串、UTF-8、数字、未知嵌套对象/数组。
4. 合法完整消息 finish 时一次性 patch 到 bambu_status_t；缺字段不清零；坏包不改变旧状态。
5. 增加 host 端自动测试目标。测试至少覆盖：全字段、单字段增量保留旧值、在每一个字节边界切包、外层/嵌套同名诱饵、转义任务名、截断/畸形包、40 KiB 未知 AMS 数组后仍有目标字段、超长 task_name 安全截断。
6. 不改桌面端现有 OpenSSL monitor，不改公共 UI。

验收：测试可独立运行且全部通过；再运行 desktop build 和 ESP all build。报告 sizeof(parser context) 和测试峰值堆（parser 自身应为 0）。验收通过后单独提交：core: add bounded Bambu status stream parser
```

### 提示词 3：ESP 云登录与 NVS profile

```text
请阅读 AGENTS.md、docs/bambu-esp32-kimi-handoff.md、src/ports/desktop/bambu_cloud_winhttp.c 和 bambu_cloud_internal.h，只做“提示词 3”。本阶段不要实现 MQTT。

目标：在 ESP32 上让现有 Bambu 设置页完成真实云登录、恢复登录、刷新设备列表和退出登录；桌面实现完全不回归。

结构：在 src/ports/esp32/bambu/ 下实现 bambu_cloud_esp32.c、bambu_runtime_esp32.c/.h、bambu_http_esp32.c/.h、bambu_secret_esp32.c/.h。单一 bambu_net actor 执行所有 HTTPS 操作，公共 API 只投递命令/读快照。

硬要求：
1. 登录流程、host、path、JSON 字段、TFA Cookie/CSRF、token/UID 提取严格对齐当前 Windows 文件，不自行改协议。
2. HTTPS 使用 esp_http_client + esp_crt_bundle_attach，校验 hostname；禁止 insecure、skip_cert_common_name_check 和硬编码跳过错误。
3. HTTP body 分块收集、上限 64 KiB；有 PSRAM优先 PSRAM，无 PSRAM用 internal heap；所有失败出口 cleanup。
4. NVS 只保存一个带 schema version 的 region/account/user_id/token profile；不保存密码、验证码、TFA key。不要声称 NVS 已加密。
5. token 上限 2048 B；密码、验证码、token 临时副本、Cookie/CSRF/TFA key 用显式不可优化的清零函数处理。
6. 日志不得输出请求 body、响应 body、Authorization、Cookie、CSRF、token、密码、验证码和完整 UID。
7. cloud snapshot 用 mutex；持锁期间不得网络、解析或 NVS I/O。logout 必须使在途 generation 失效并清空 NVS/RAM。
8. ESP core 构建改用真实 cloud port，但 monitor 继续使用 stub。Windows CMake 和源文件不要重构。
9. 若内存不足要返回可理解的 FAILED message，不能 assert、死循环或静默。

验收：desktop 和 ESP all 构建。真机测试需要用户自己在屏幕输入账号，绝对不要索要、记录或回显用户密码/token。至少给出 CYD 与一块 S3 在登录前、TLS 期间、登录完成清理后的 mem 数据，并验证重启恢复登录、刷新列表、logout 后重启仍为退出。无法做真实账号步骤时明确标成“待用户真机验证”，不要宣称完成。通过后提交：esp32: add Bambu Cloud authentication
```

### 提示词 4：ESP MQTT Cloud Monitor

```text
请阅读 AGENTS.md、docs/bambu-esp32-kimi-handoff.md、src/ports/desktop/bambu_monitor_openssl.c 和上一阶段代码，只做“提示词 4”。不要做 LAN 或任何控制命令。

目标：实现 ESP32 Bambu Cloud MQTT 只读实时监视，并与同一个 bambu_net actor 共享生命周期。

要求：
1. 使用 ESP-IDF 自带 esp-mqtt，MQTT 3.1.1 over TLS；broker、username、topic、pushall 按计划文档。
2. 证书使用 esp_crt_bundle_attach 且校验 hostname；不得 insecure。
3. RX 4096 B、TX 1024 B 起步；按 total_data_len/current_data_offset 校验连续分片，并逐块喂 bambu_status_stream，禁止累计完整 payload，禁止在 ESP 路径对 report 调 cJSON_Parse。
4. 单条上限 49152 B；topic 只在首片存在，必须记录并验证目标 report topic；坏 offset、换 topic、超限、解析失败都丢弃整条，不污染旧状态。
5. connected 后订阅 report，确认订阅请求已接受后发布一次 pushall；同一 serial 的 pushall 最短间隔 5 分钟，快速重连不能轰炸。
6. HTTP 操作优先：有登录/刷新命令时 actor 先完整 stop+destroy MQTT，释放 TLS 后做 HTTP，结束后按 desired serial 恢复 MQTT。任何时刻最多一套 Bambu TLS 会话。
7. event callback 不做阻塞销毁、不调 LVGL；monitor snapshot 加锁更新。认证拒绝映射 AUTH_ERROR，普通断网映射 NETWORK_ERROR，并使用有上限的退避。
8. stop/start 幂等；切换 serial、logout、切回 Klipper、Wi-Fi 掉线都不能留下旧 client 或 stale callback。generation 必须覆盖这些场景。
9. token 只由 runtime 的受控 credential copy 提供，用完的 client 配置副本清零；日志只允许 serial 的末 4 位，不能完整打印 UID/token。
10. 把 ESP 构建从 monitor stub 切到真实实现，桌面 OpenSSL 实现保持不动，capabilities 继续为 0。

验收：先跑 stream parser tests、desktop build、ESP all build。真机确认温度/状态/任务/进度/层数能更新；记录首个大 pushall 的 total length、是否分片、解析成功与否，但不要记录 payload。无法真机验证就明确停止在待验证状态，不得顺手做后续功能。通过后提交：esp32: add Bambu Cloud monitor
```

### 提示词 5：只做稳定性与内存验收

```text
请阅读 AGENTS.md 和 docs/bambu-esp32-kimi-handoff.md，只做“提示词 5”。本阶段不增加功能、不重构 UI、不做 LAN。

目标：证明 Cloud Monitor 在 CYD 和 S3 上不会泄漏、不会与 Moonraker 并存、不会因大包 OOM。

按计划文档第 5 节执行压力测试。补一个不泄露秘密的串口 bambu 状态命令也可以，输出仅限 cloud/monitor state、desired serial 末 4 位、MQTT client 是否存在、当前消息 total/offset、成功/丢弃计数、actor 和 MQTT 栈 high-water；禁止任何账号/token/UID/Cookie。

逐项记录：登录前后、MQTT 首包、10/30 分钟、20 次页面切换、20 次 Klipper/Bambu 切换、5 次 Wi-Fi 断开恢复的 free/min/largest、taskmem。发现趋势先用 ht 定位具体调用点，只有证据支持才修改代码。

禁止用增大看门狗、吞 assert、关闭证书校验、减少测试次数、重启掩盖泄漏。禁止无证据批量改 sdkconfig；确需修改时同时改 defaults 与已生成 canonical 行。

验收：desktop + ESP all build，列出测试板、持续时间、每个阶段数值、是否达到计划门槛、仍未覆盖的账号区域/机型。未达到就继续修本阶段，不提交“完成”。通过后单独提交：esp32: harden Bambu monitor lifecycle
```

### 提示词 6：收口 UI、文档与最终回归

```text
请阅读 AGENTS.md 和 docs/bambu-esp32-kimi-handoff.md，只做“提示词 6”。

目标：把已验证的 ESP Cloud Monitor 收口为可发布功能，但本阶段不改版本号、不打 tag。

要求：
1. panel_bambu_link.c 的 LAN Developer Mode 仍未实现，必须显示为未实现并禁止选中；不能让用户切过去后得到黑洞状态。
2. panel_bambu_setup.c 在页面销毁时清零静态 password/code，并遵守“非主面板不常驻”约定；不要增加常驻页面或大静态缓冲。
3. 新增错误文案走稳定 message key，并补齐 src/ui/lang.c 五语言；未知服务端原文只做安全、限长展示，不能包含秘密。
4. 更新中英文用户文档：支持范围、登录方式、只读边界、token 存在 NVS 且未启用 flash encryption 时可被物理读取、退出登录清除凭据、目前不支持 LAN/控制。
5. 不改 Windows 已验证流程，不开放任何 capability，不添加假 AMS/摄像头入口。

最终验收：stream parser tests、desktop build、ESP all build、git diff --check；再复核 git diff 中没有 token、账号、抓包 payload、串口日志或本机绝对路径。汇总所有阶段真机证据和已知限制。通过后单独提交：docs: finalize ESP Bambu Cloud monitor
```

## 7. 审查时的红线清单

出现以下任一项就不要进入下一阶段：

- 在 LVGL/timer callback 里直接做阻塞网络销毁；
- Moonraker WebSocket 与 Bambu MQTT 同时存在；
- ESP report 路径仍把 30–48 KiB 原文交给 cJSON DOM；
- token/password 出现在 `ESP_LOG*`、CLI、配置文本、core dump 辅助输出或提交内容；
- TLS 关闭证书/主机名校验；
- MQTT 分片未校验 offset/topic/total length；
- stop 后 stale callback 能把快照重新写成 connected；
- 网络失败导致 UI 线程阻塞、assert 或自动重启；
- 只构建一块 S3，就宣称 CYD 正式支持；
- 一个提交同时混入 LAN、控制、UI 重构或 sdkconfig 大改。

## 8. 推荐的人机协作节奏

每完成一条提示词，让 Kimi 返回以下四样：

1. 提交 hash 与文件列表；
2. `git show --stat` 和关键 diff；
3. 完整构建结论；
4. 真机串口中脱敏后的内存/状态证据。

把这四样交给 Codex 做一次只读审查。审查只盯当前阶段，通常比让 Codex直接重写整块代码省很多额度，也更容易在错误刚出现时拦住。
