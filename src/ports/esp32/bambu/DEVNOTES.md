# Bambu Cloud 登录（ESP32）实现与踩坑记录

> 内部开发笔记，面向后续维护者（提示词 4/5/6 的接手人）。不面向用户，不进 mkdocs。
> 状态：提示词 1–3 已完成并真机验证（CYD 短信登录全通）。monitor 仍是 stub。

## 1. 分层与文件

```
UI (panel_bambu_setup.c / panel_moonraker.c / printer_model.c)
  │  只用公共 API，语义与桌面完全一致
  ▼
src/core/bambu_cloud.h                     公共异步 API（快照 + 命令），全平台同一份
src/ports/esp32/bambu/bambu_cloud_esp32.c  纯转发薄封装
bambu_runtime_esp32.c/.h                   bambu_net actor 任务 + 快照 + 生命周期 + 协议状态机
bambu_http_esp32.c/.h                      有界 HTTPS（esp_http_client + crt_bundle）+ Cookie jar
bambu_secret_esp32.c/.h                    NVS 单 profile（kr_bambu/profile，schema=1 固定布局 blob）
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

- 单 actor（`bambu_net`，栈 10KiB，队列深 4 传堆对象指针）串行执行所有 HTTPS。
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
- 缺 PSRAM 的板子上任何"大 TLS 会话 + LVGL 同帧渲染"组合都有 OOM 风险，
  提示词 4 的 MQTT over TLS 上线时要复用本轮的内存账本重新核对一遍。

## 5. 当前边界与待办

- monitor 仍是 `bambu_monitor_stub.c`（提示词 4 才接 esp-mqtt）；capabilities=0。
- 真机已验证（CYD，中国区）：短信验证码登录全链路、token 存 NVS、堆栈水位。
  密码登录/邮箱码/TFA/全球区/重启恢复/logout 后重启仍为退出——按 handoff 验收
  清单仍属"待用户真机验证"，不要在文档里宣称完成。
- `bambu_cloud_copy_mqtt_credentials` / `resolve_mqtt_user_id`（desktop 的
  internal 头）ESP 端刻意未实现，提示词 4 时按 token 受控拷贝的要求设计。
- 压力测试（提示词 5）要做：20 次页面切换、20 次 Klipper/Bambu 切换、5 次
  WiFi 断开恢复的 free/min/largest + taskmem；ht 采样用 128 条/1–2s（512 条×10s
  会把自己压死）。

## 6. 相关提交

- `21856a1` core: add bounded Bambu status stream parser（提示词 2）
- `1e5edb0` esp32: add Bambu Cloud authentication（提示词 3 主体）
- `522970a` esp32: stabilize Bambu cloud login on non-PSRAM boards（4.3+4.4）
- 计划文档：`docs/bambu-esp32-kimi-handoff.md`（6 条提示词与红线清单）、
  `docs/bambu-integration-architecture.md`
