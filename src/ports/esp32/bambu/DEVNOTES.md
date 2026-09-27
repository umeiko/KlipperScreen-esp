# Bambu Cloud 登录（ESP32）实现与踩坑记录

> 内部开发笔记，面向后续维护者。不面向用户。
> 状态：CYD 中国区短信登录与 Cloud MQTT 只读监视均已真机跑通。

## 1. 分层与文件

```
UI (panel_bambu_setup.c / panel_moonraker.c / printer_model.c)
  │  只用公共 API，语义与桌面完全一致
  ▼
src/core/bambu_cloud.h                     公共异步 API（快照 + 命令），全平台同一份
src/ports/esp32/bambu/bambu_cloud_esp32.c  纯转发薄封装
bambu_runtime_esp32.c/.h                   bambu_net actor 任务 + 快照 + 生命周期 + 协议状态机
bambu_monitor_esp32.c                      esp-mqtt + 分片流式状态合并
bambu_monitor_esp32_internal.h             monitor 的 actor 内部钩子
bambu_http_esp32.c/.h                      有界 HTTPS（esp_http_client + crt_bundle）+ Cookie jar
bambu_secret_esp32.c/.h                    NVS 单 profile（kr_bambu/profile，schema=1 固定布局 blob）
src/core/bambu_status_stream.c/.h           48KiB 上限、368B context、零堆解析器
```

协议基准是 `src/ports/desktop/bambu_cloud_winhttp.c`（979 行）。host、path、JSON
字段、TFA Cookie/CSRF 顺序、token/UID 提取、设备解析、错误文案**逐行对齐它**，
改协议必须先改 Windows 再镜像过来。

## 2. 协议要点（照抄 Windows 版，勿自行发挥）

- host：api=`api.bambulab.cn|com`，site=`bambulab.cn|com`；UA=`bambu_network_agent/01.09.05.01`。
- 密码登录：`POST api/v1/user-service/user/login`，body `{"account","password"}`。
- 邮箱码：`POST .../user/sendemail/code` `{"email","type":"codeLogin"}`；
  短信码（仅中国区）：`POST .../user/sendsmscode` `{"phone","type":"codeLogin"}`。
- 提交验证码：`POST login` `{"account","code"}`。
- TFA：先 `GET site/api/csrf`（从 Set-Cookie 取 `bbl_csrf_token`），再
  `POST site/api/sign-in/tfa` `{"tfaKey","tfaCode"}`，带 Cookie 头 + `x-bbl-csrf-token` 头。
- 设备列表：`GET site/api/v1/iot-service/api/user/bind`（Bearer）；UID fallback：
  `GET api/v1/user-service/my/profile`（Bearer）。
- token：`accessToken`/`token`，根或 `data` 下；TFA 应答 token 也可能在 Cookie 的 `token` 里。
- UID：JWT 中段 base64url → JSON 取 `uidStr/uid/sub/userId/user_id`，无 `u_` 前缀补上。
- 设备：`devices` 数组（根或 data）取 `dev_id`（必需）/`name`/`dev_product_name||dev_model_name`/`online`，上限 12。
- 错误文案：json `error`/`message`，否则 `兜底（HTTP xxx）`；UI 按"（"前的精确 key 查
  lang.c 五语言表，后缀原样保留（panel_bambu_setup.c `localized_cloud_message`）。
  **新增文案必须是稳定 key 并补 lang.c 五语言**，否则非中文界面显示原文。

## 3. 生命周期与并发约定

- 单 actor（`bambu_net`，栈 10KiB，队列深 4 传堆对象指针）串行编排 HTTPS 与
  MQTT 生命周期。HTTP 命令执行前先完整停止/销毁 MQTT，结束后再恢复，CYD 上
  不允许两套 TLS 同时存活。
  公共 API 只做：校验参数 → calloc cmd → 置 BUSY → xQueueSend。
- `g_outstanding`（mutex 内）镜像 Windows `g_busy`：同时只允许一个在途操作，
  忙时公共 API 返回 false（UI  toast "登录任务正在运行"）。
- `g_generation`：logout 时 ++，actor 发布任何状态前检查，挡陈旧发布。
- logout 的 NVS 清除走队列里一条 `OP_ERASE_SECRET` 命令——排在在途 login 之后，
  防止"generation 检查通过 → logout → 在途 op 的 secret_save 又写回"的交错。
  队列不可用（init 失败/满）时退化为调用方内联擦除（NVS 擦除是毫秒级 flash 操作，允许）。
- 快照/凭据（g_token/g_user_id/g_tfa_key）只被静态 mutex 保护做纯内存拷贝；
  **持锁期间禁止网络/JSON/NVS I/O**。
- `bambu_rt_init` 懒初始化（面板 create 时首次调用），当前只在 LVGL 线程被调，
  无并发初始化保护——新增调用方时注意。
- NVS：namespace `kr_bambu`（≤15 字符），key `profile`，固定布局
  `{schema, region, account[128], user_id[96], token[2048]}`。读取校验
  size/schema/NUL/非空，坏数据=未登录。**未加密**（flash encryption 未开时可物理读出），
  用户文档要如实说明；logout=nvs_erase_all。不存密码/验证码/TFA key。
- 秘密卫生：密码/token/CSRF/cookie 临时副本用完 `bambu_http_wipe`（volatile 清零）；
  日志只打 op 编号/堆/栈水位，禁打 body、Authorization、Cookie、token、完整 UID。

## 4. 踩过的坑（本轮实锤，按发现顺序）

### 4.1 静态库链接顺序（CYD 首构建即挂）
`ui`/`core` 引用 `bambu` 组件符号，但 bambu REQUIRES core → libbambu.a 排在
libcore/libui 前面，undefined reference 一整屏。修法：`ui` 和 `core` 的
REQUIRES 都加 `bambu`——CMake 对静态库循环依赖会自动重复库名，IDF 组件图允许成环。

### 4.2 S3 的 -Werror=stringop-truncation
Windows 式 `strncpy(dst, src, cap-1)` 在 GCC 能看到两侧固定大小时直接 -Werror
（esp32 目标不报，esp32s3 报——两边 warning 配置不同，CYD 过了不代表 S3 能过，
**验收必须跑全部板型**）。修法：`copy_text` 改 strlen+memcpy 实现。

### 4.3 CYD 点"短信验证码"死机重启（本阶段最大的坑）
现象：Guru Meditation LoadProhibited，EXCVADDR=0x26，backtrace 全在 LVGL
渲染（`get_prop_core` 样式遍历），与 actor 无关——**不是协议代码写坏内存，是
TLS 握手把堆压到临界后 LVGL 某处内存申请失败**（LVGL 对 malloc 失败并不健壮）。
堆账本（CYD，无 PSRAM）：进面板后 actor 栈 10KB + 面板/键盘对象，点按钮时只剩
~67KB；mbedTLS 默认 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384`，握手瞬时 ~40KB。
修法（与 4.4 叠加后实测通过）：三块**无 PSRAM** 板（cyd_2432s028r / e32r35t /
ec11_knob_esp32）IN 记录缓冲 16384→4096，TLS 分片由协议层自动处理，对 API 型
HTTPS 足够。**S3 板不动**。
- **sdkconfig 双改规则再强调**：defaults 和 canonical 都要改；
  且 CYD 的 canonical `src/ports/esp32/sdkconfig` 在 .gitignore 里（不入库，
  只改本地构建用），入库的是 `sdkconfig.defaults.<board>` 和 `sdkconfig.<board>`。
- 诊断手法：串口抓 backtrace → `xtensa-esp-elf-addr2line -pfiaC -e build/klipper_remote_display.elf <地址...>`。

### 4.4 actor 栈帧合并导致的栈水位告急（GPT 修复，已实测）
原 `run_command` 一个函数承载所有分支，编译器按最大分支保留栈帧
（TFA 分支 csrf[1024]+token_cookie[2048]+devices[~1.9KB]），叠加 http 层
url/chunk/auth 缓冲，完整短信登录最低余栈只有 1564B。修法：
- refresh / login_start / submit_code / submit_tfa 拆成 `__attribute__((noinline))`
  独立帧，互斥分支不再共享栈空间（run_command 帧 2112B→32B）；
- HTTP 层去掉 1KB 中转数组，`esp_http_client_read` 直读响应缓冲；
  Authorization 头从 2100B 栈数组改按需 malloc（用完 wipe+free）；
- Cookie jar（4KB）从 `bambu_http_result_t` 移出，仅 TFA 流程由调用方堆分配
  （result 结构有 `_Static_assert(≤16B)` 守住，别再往里塞大数组）；
- body_grow 按 used 而非 old_cap 拷贝。
实测：登录全程最低余栈 3624B（>3KB 安全线，actor 栈维持 10KiB 不加）；
TLS 期间堆低 ~37.8KB，结束回落 ~93.4KB，30s 无泄漏。

### 4.5 历史遗留（未修，记在案）
- `bsp_wifi_esp32.c` 连接时把 WiFi 密码明文+hex 打进串口日志。

### 4.6 Bambu 模式开机在 UI 创建阶段无限重启
`printer_state()` 会在 `ui_app_create()` 的深调用链中触发 monitor/runtime 懒初始化。
原实现把约 2.3KiB 的 `bambu_secret_t` 放在只有 3.5KiB 的 main task 栈上，尚未启动
Wi-Fi 就报 `A stack overflow in task main`。Klipper 模式初始化较晚、栈较浅，因此
此前未暴露。修法是 NVS profile 临时对象改为堆分配，读取后显式清零并释放。

### 4.7 MQTT 每约 15 秒 `-0x7100` 断线重连
CYD 为保住登录器内存把 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 降到 4096，但仅缩小
本地 buffer 并不会自动要求服务器缩小 TLS record。Bambu 下发大 record 时，
`mbedtls_ssl_fetch_input()` 因记录装不下返回 `MBEDTLS_ERR_SSL_BAD_INPUT_DATA`
（`-0x7100`）。不能简单把全局缓冲改回 16KiB，否则会重新触发登录阶段 OOM。

修法：MQTT 专用的 crt-bundle attach wrapper 在保留证书链和 hostname 校验的同时，
对该连接调用 `mbedtls_ssl_conf_max_frag_len(..., 4096)`，通过 RFC 6066 MFL 要求
broker 分片。中国区与全球区 broker 均用 OpenSSL 实测回送 MFL=4096。CYD 实机
最终版连续 125 秒无断线/重连，free heap 在 52120–52476B 间波动，min-ever
固定为 44636B，largest block=45044B；状态 getter 读到喷嘴温度 31.8→31.7°C，
证实增量持续更新。report topic 中不含目标打印字段的心跳、AMS/摄像头通知属于
正常忽略项，不应打 warning。

### 4.8 Bambu 模式主页时钟一直 `--:--`
标题栏只读系统时钟；此前 ESP32 的唯一校时入口由 Moonraker 启动路径触发。切到
Bambu 后 Moonraker 正确停用，也就失去了时间来源。修法不是另起 NTP/TLS 连接，
而是在 `bambu_http_esp32.c` 的现有 HTTPS 响应回调里读取标准 `Date` 头，复用 BSP
通用解析入口设置系统时间。这样重启恢复登录时的设备列表刷新会顺便校时，且不会
与 MQTT 同时增加第二套 TLS 堆占用。

## 5. 当前边界与待办

- ESP monitor 已换成 `bambu_monitor_esp32.c`：MQTT 3.1.1/TLS、订阅 report、限频
  pushall、断线退避、连续分片校验与零堆状态合并；capabilities 仍为 0（只读）。
- 真机已验证（CYD，中国区）：短信验证码登录全链路、token 存 NVS、堆栈水位。
  密码登录/邮箱码/TFA/全球区/重启恢复/logout 后重启仍为退出——按 handoff 验收
  清单仍属"待用户真机验证"，不要在文档里宣称完成。
- 压力测试（提示词 5）要做：20 次页面切换、20 次 Klipper/Bambu 切换、5 次
  WiFi 断开恢复的 free/min/largest + taskmem；ht 采样用 128 条/1–2s（512 条×10s
  会把自己压死）。

## 6. 相关提交

- `21856a1` core: add bounded Bambu status stream parser（提示词 2）
- `1e5edb0` esp32: add Bambu Cloud authentication（提示词 3 主体）
- `522970a` esp32: stabilize Bambu cloud login on non-PSRAM boards（4.3+4.4）
- 计划文档：`docs/bambu-esp32-kimi-handoff.md`（6 条提示词与红线清单）、
  `docs/bambu-integration-architecture.md`
