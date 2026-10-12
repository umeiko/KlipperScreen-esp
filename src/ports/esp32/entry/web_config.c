/*
 * Web 配置后台：esp_http_server 提供手机配置页
 *  - GET  /                 配置页面（HTML）
 *  - GET  /api/status       当前配置 JSON（WiFi + 全 6 槽位打印机 + 屏保延时 + 固件版本）
 *  - POST /api/config       保存 WiFi/打印机（slot 参数）/屏保延时，3 秒后自动重启生效
 *  - POST /api/ota          上传 .bin 固件 OTA 升级（流式写另一槽位，校验后重启）
 * 启动时若无 WiFi 配置，自动开启 SoftAP 热点（KlipperScreen-AP / 12345678），
 * 手机连接热点后访问 http://192.168.4.1 完成首次配置。
 * 所有耗时操作都在 httpd 任务内执行，不影响 LVGL UI 任务。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "app_settings.h"
#include "bsp_wifi.h"

#define TAG "webcfg"

#define AP_SSID    "KlipperScreen-AP"
#define AP_PASS    "12345678"

static httpd_handle_t s_server;

/* ---------------- HTML 页面 ---------------- */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>KlipperScreen 配置</title><style>"
    "body{font-family:system-ui,sans-serif;background:#12151c;color:#e8eaed;margin:0;padding:16px;max-width:560px}"
    "h1{font-size:20px;margin:4px 0 16px}h2{font-size:15px;margin:20px 0 8px;color:#2d9cdb;border-bottom:1px solid #262c3a;padding-bottom:6px}"
    "label{display:block;font-size:13px;margin:10px 0 4px;color:#8b93a1}input,select{width:100%;box-sizing:border-box;padding:10px;"
    "background:#1e232e;color:#e8eaed;border:1px solid #3a4152;border-radius:8px;font-size:15px}"
    "button{margin-top:16px;width:100%;padding:12px;background:#2d9cdb;color:#fff;border:0;border-radius:8px;font-size:15px;font-weight:600}"
    "button.sec{background:#262c3a;color:#8b93a1;border:1px solid #3a4152}"
    "button.danger{background:#e74c3c;color:#fff}"
    ".tip{font-size:12px;color:#8b93a1;margin-top:8px}"
    ".upbtn{display:block;margin:6px 0;padding:10px;border:1px solid #3a4152;border-radius:8px;background:#1f2733;color:#dce6f0;text-align:center;font-size:13px;font-weight:600}"
    ".row{display:flex;gap:8px;margin:12px 0}.row button{flex:1;margin:0;padding:10px;font-size:14px}"
    "#status{margin-top:12px;padding:10px;border-radius:8px;display:none;font-size:13px}"
    "#status.ok{display:block;background:#1b2e22;color:#7ee2a8}#status.err{display:block;background:#3a1d1d;color:#f5a3a3}"
    "</style></head><body>";

static const char PAGE_FORM[] =
    "<h1>KlipperScreen 后台配置</h1>"
    "<h2>WiFi 网络</h2>"
    "<label>WiFi 名称</label><input id='ssid' maxlength='32' placeholder='CMCC-bSHp'>"
    "<label>WiFi 密码</label><input id='pass' maxlength='63' placeholder='留空则不修改'>"
    "<h2>打印机槽位（1~6）</h2>"
    "<div class='row' id='slotrow'>"
    "<button class='sec' onclick='pick(0)'>1</button><button class='sec' onclick='pick(1)'>2</button>"
    "<button class='sec' onclick='pick(2)'>3</button><button class='sec' onclick='pick(3)'>4</button>"
    "<button class='sec' onclick='pick(4)'>5</button><button class='sec' onclick='pick(5)'>6</button></div>"
    "<label>工作模式</label><select id='mode'><option value='klipper'>Klipper / Moonraker</option><option value='bambu'>Bambu 拓竹</option></select>"
    "<label>打印机名称</label><input id='name' maxlength='47' placeholder='可选'>"
    "<label>主机 IP 地址</label><input id='host' maxlength='63' placeholder='192.168.10.198'>"
    "<label>端口</label><input id='port' maxlength='5' placeholder='7125'>"
    "<label>API Key（可留空）</label><input id='api_key' maxlength='63'>"
    "<h2>显示</h2>"
    "<label>屏保延时（秒，1~60）</label><input id='delay' type='number' min='1' max='60'>"
    "<h2>天气时钟（高德天气）</h2>"
    "<div class='tip'>需高德开放平台免费 Key（lbs.amap.com 注册→控制台→创建应用→添加 Key，选 Web服务）。城市代码填 adcode（渭南=610500）。</div>"
    "<label>高德天气 Key</label><input id='wkey' maxlength='63' placeholder='Web服务 Key'>"
    "<label>城市代码 adcode</label><input id='wcity' maxlength='15' placeholder='610500'>"
    "<label>城市显示名</label><input id='wcityname' maxlength='15' placeholder='渭南'>"
    "<button onclick='saveCfg()'>保存并重启</button>"
    "<div id='status'></div>"
    "<h2>固件升级（OTA）</h2>"
    "<div class='tip'>当前版本 <span id='fw'>?</span>。开机自动检测：服务器目录放 version.txt（版本串）与 firmware.bin，版本不同自动升级。也可下方手动上传 .bin 立即升级。升级期间请勿断电。</div>"
    "<label>OTA 服务器地址</label><input id='otaurl' maxlength='150' placeholder='http://192.168.10.200:8080/fw'>"
    "<input type='file' id='fwfile' accept='.bin'>"
    "<button onclick='otaUp()'>上传并升级</button>"
    "<div id='otastatus'></div>";

static const char PAGE_JS[] =
    "<script>"
    "var SLOT=0;var J=null;var E={};"
    "function g(id){return document.getElementById(id).value}"
    "function s_(id,v){document.getElementById(id).value=v}"
    "function hl(){var bs=document.getElementById('slotrow').children;"
    "for(var i=0;i<bs.length;i++){bs[i].style.background=(i===SLOT)?'#2d9cdb':'#262c3a'}}"
    "function show(t,ok){var s=document.getElementById('status');s.className=ok?'ok':'err';"
    "s.textContent=t;setTimeout(function(){s.style.display='none'},6000)}"
    "function fill(){if(!J||!J.slots)return;var m=J.slots[SLOT]||{};"
    "s_('mode',m.mode||'klipper');s_('name',m.name||'');s_('host',m.host||'');"
    "s_('port',m.port||7125);s_('api_key',m.api_key||'');hl()}"
    "function pick(i){E[SLOT]={mode:g('mode'),name:g('name'),host:g('host'),port:g('port'),api_key:g('api_key')};"
    "SLOT=i;var v=E[i]||(J&&J.slots[i])||{};"
    "s_('mode',v.mode||'klipper');s_('name',v.name||'');s_('host',v.host||'');"
    "s_('port',v.port||7125);s_('api_key',v.api_key||'');hl()}"
    "async function saveCfg(){var b=new URLSearchParams();"
    "b.set('slot',SLOT);"
    "b.set('ssid',document.getElementById('ssid').value);"
    "b.set('pass',document.getElementById('pass').value);"
    "b.set('mode',document.getElementById('mode').value);"
    "b.set('name',document.getElementById('name').value);"
    "b.set('host',document.getElementById('host').value);"
    "b.set('port',document.getElementById('port').value);"
    "b.set('api_key',document.getElementById('api_key').value);"
    "b.set('delay',document.getElementById('delay').value);"
    "b.set('wkey',document.getElementById('wkey').value);"
    "b.set('wcity',document.getElementById('wcity').value);"
    "b.set('wcityname',document.getElementById('wcityname').value);"
    "b.set('otaurl',document.getElementById('otaurl').value);"
    "try{var r=await fetch('/api/config',{method:'POST',body:b});"
    "show(await r.text(),r.ok);if(r.ok)show('保存成功，3 秒后自动重启…',true)}"
    "catch(e){show('网络错误: '+e,false)}}"
    "async function load(){try{var r=await fetch('/api/status');J=await r.json();"
    "document.getElementById('ssid').value=J.ssid||'';"
    "document.getElementById('pass').value=J.pass||'';"
    "document.getElementById('delay').value=J.delay||5;"
    "document.getElementById('wkey').value=J.wkey||'';"
    "document.getElementById('wcity').value=J.wcity||'';"
    "document.getElementById('wcityname').value=J.wcityname||'';"
    "document.getElementById('otaurl').value=J.otaurl||'';"
    "document.getElementById('fw').textContent=J.fw||'?';"
    "SLOT=(J.active||0);fill();"
    "document.getElementById('slotrow').children[SLOT].style.background='#2d9cdb'}catch(e){}}"
    "async function otaUp(){var f=document.getElementById('fwfile').files[0];"
    "if(!f){show('请先选择 .bin 固件文件',false);return}"
    "if(!confirm('确认升级固件？升级期间请勿断电或关闭页面'))return;"
    "show('上传中，请保持页面打开…',true);"
    "try{var r=await fetch('/api/ota',{method:'POST',body:f});"
    "var t=await r.text();show(t,r.ok);"
    "if(r.ok)setTimeout(function(){location.href='/'},6000)}"
    "catch(e){show('上传失败: '+e,false)}}"
    "load();</script></body></html>";

/* ---------------- 工具 ---------------- */

static void url_decode(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    while (*src && o + 1 < dstsz) {
        if (*src == '%' && src[1] && src[2]) {
            char h[3] = { src[1], src[2], 0 };
            dst[o++] = (char)strtol(h, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

/* JSON 字符串转义（引号/反斜杠），防止用户输入破坏 /api/status 结构 */
static void json_escape(char *out, size_t outsz, const char *in)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < outsz; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 1 < outsz) { out[o++] = '\\'; }
            if (o + 1 < outsz) { out[o++] = *p; }
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* 解析 a=b&c=d...，把指定 key 的值 url_decode 到 out（缺省返回 false） */
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vlen = seg - kl - 1;
            char tmp[256];
            size_t tl = vlen < sizeof(tmp) ? vlen : sizeof(tmp) - 1;
            memcpy(tmp, p + kl + 1, tl);
            tmp[tl] = 0;
            url_decode(out, outsz, tmp);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/* ---------------- HTTP 处理 ---------------- */

static void log_req(httpd_req_t *req)
{
    ESP_LOGI(TAG, "REQ %s %s", req->method == HTTP_GET ? "GET" : "POST", req->uri);
}
static esp_err_t handle_root(httpd_req_t *req)
{
    log_req(req);
    /* 三段分块发送：不占 10KB 静态缓冲（内部 RAM 紧张），httpd 自动算总长度 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");   /* 防手机缓存旧页面 */
    esp_err_t e = httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, PAGE_FORM, HTTPD_RESP_USE_STRLEN);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, PAGE_JS, HTTPD_RESP_USE_STRLEN);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);   /* 结束块 */
    ESP_LOGI(TAG, "PAGE send: %s", esp_err_to_name(e));
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    log_req(req);
    wifi_conf_t w;
    static char buf[3072];   /* httpd 单线程串行处理，静态复用；避免占任务栈 */
    settings_load_wifi(&w);
    int delay = settings_load_saver_delay();
    int active = settings_load_active_printer();
    char wk[64], wc[32], wn[16], esc_wk[128], esc_wc[64], esc_wn[32];
    settings_load_weather_key(wk, sizeof(wk));
    settings_load_weather_city(wc, sizeof(wc));
    settings_load_weather_city_name(wn, sizeof(wn));
    json_escape(esc_wk, sizeof(esc_wk), wk);
    json_escape(esc_wc, sizeof(esc_wc), wc);
    json_escape(esc_wn, sizeof(esc_wn), wn);
    char ou[160], esc_ou[320];
    settings_load_ota_url(ou, sizeof(ou));
    json_escape(esc_ou, sizeof(esc_ou), ou);
    char esc_ssid[64], esc_pass[128];
    json_escape(esc_ssid, sizeof(esc_ssid), w.ssid);
    json_escape(esc_pass, sizeof(esc_pass), w.pass);
    const esp_app_desc_t *app = esp_app_get_description();
    const char *fw = (app && app->version[0]) ? app->version : "?";
    char *p = buf;
    int n = snprintf(p, sizeof(buf) - (size_t)(p - buf),
                     "{\"ssid\":\"%s\",\"pass\":\"%s\",\"delay\":%d,\"active\":%d,\"fw\":\"%s\",\"wkey\":\"%s\",\"wcity\":\"%s\",\"wcityname\":\"%s\",\"otaurl\":\"%s\",\"slots\":[",
                     esc_ssid, esc_pass, delay, active, fw, esc_wk, esc_wc, esc_wn, esc_ou);
    p += n;
    for (int i = 0; i < PRINTER_SLOTS; i++) {
        moonraker_conf_t m;
        settings_load_moonraker_slot(i, &m);
        machine_mode_t mode = settings_load_machine_mode_slot(i);
        n = snprintf(p, sizeof(buf) - (size_t)(p - buf),
                     "%s{\"mode\":\"%s\",\"name\":\"%s\",\"host\":\"%s\",\"port\":%u,\"api_key\":\"%s\"}",
                     i ? "," : "", mode == MACHINE_MODE_BAMBU ? "bambu" : "klipper",
                     m.name, m.host, m.port, m.api_key);
        p += n;
    }
    snprintf(p, sizeof(buf) - (size_t)(p - buf), "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* ---------------- OTA 升级 ----------------
 * POST /api/ota：body 直接为 .bin 固件（application/octet-stream）。
 * httpd 流式接收 → esp_ota_write 写另一槽位 → esp_ota_end 校验 →
 * set_boot_partition → 1 秒后重启。失败自动 abort，不影响当前固件。
 */
#define OTA_BUF_SIZE 4096

static esp_err_t handle_ota(httpd_req_t *req)
{
    log_req(req);
    ESP_LOGI(TAG, "OTA start, content_len=%d", req->content_len);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA target: %s @0x%x size=%d", part->label,
             (unsigned)part->address, (int)part->size);

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }

    static char buf[OTA_BUF_SIZE];   /* httpd 单线程处理，可静态复用 */
    int len, total = 0;
    while ((len = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        err = esp_ota_write(handle, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return ESP_OK;
        }
        total += len;
    }
    if (len < 0) {  /* 读错误/超时；recv 返回 0 表示 body 已读尽 */
        ESP_LOGE(TAG, "upload interrupted, recv=%d after %d bytes", len, total);
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA received %d bytes", total);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end image invalid: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "firmware image invalid");
        return ESP_OK;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA success, rebooting to %s", part->label);
    httpd_resp_sendstr(req, "OTA OK, rebooting...");
    esp_timer_handle_t t;
    if (esp_timer_create(&(esp_timer_create_args_t){ .callback = reboot_cb, .arg = NULL }, &t) == ESP_OK) {
        esp_timer_start_once(t, 1 * 1000 * 1000);
    }
    return ESP_OK;
}

static esp_err_t handle_config(httpd_req_t *req)
{
    log_req(req);
    char body[2048];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_OK; }
    body[len] = 0;

    char v[128];
    bool dirty = false;

    /* 槽位（缺省当前槽） */
    int slot = settings_load_active_printer();
    if (form_get(body, "slot", v, sizeof(v)) && v[0]) {
        int s = atoi(v);
        if (s >= 0 && s < PRINTER_SLOTS) slot = s;
    }
    settings_save_active_printer(slot);

    wifi_conf_t w;
    settings_load_wifi(&w);
    if (form_get(body, "ssid", v, sizeof(v)) && v[0]) { strncpy(w.ssid, v, sizeof(w.ssid) - 1); w.ssid[sizeof(w.ssid) - 1] = 0; w.valid = true; dirty = true; }
    if (form_get(body, "pass", v, sizeof(v)) && v[0]) { strncpy(w.pass, v, sizeof(w.pass) - 1); w.pass[sizeof(w.pass) - 1] = 0; w.valid = true; dirty = true; }
    if (dirty) {
        if (settings_save_wifi(&w)) {
            wifi_conf_t chk;
            bool ok = settings_load_wifi(&chk);
            ESP_LOGI(TAG, "wifi saved: ssid='%s' pass_len=%d readok=%d", chk.ssid,
                     (int)strlen(chk.pass), ok ? 1 : 0);
        } else {
            ESP_LOGE(TAG, "wifi save FAILED (littlefs write)");
        }
    }

    moonraker_conf_t m;
    settings_load_moonraker_slot(slot, &m);
    bool mdirty = false;
    if (form_get(body, "mode", v, sizeof(v))) {
        settings_save_machine_mode_slot(slot, strcmp(v, "bambu") == 0 ? MACHINE_MODE_BAMBU : MACHINE_MODE_KLIPPER);
    }
    if (form_get(body, "name", v, sizeof(v)) && v[0]) { strncpy(m.name, v, sizeof(m.name) - 1); m.name[sizeof(m.name) - 1] = 0; m.valid = true; mdirty = true; }
    if (form_get(body, "host", v, sizeof(v)) && v[0]) { strncpy(m.host, v, sizeof(m.host) - 1); m.host[sizeof(m.host) - 1] = 0; m.valid = true; mdirty = true; }
    if (form_get(body, "port", v, sizeof(v)) && v[0]) { int p = atoi(v); if (p > 0 && p < 65536) { m.port = (uint16_t)p; m.valid = true; mdirty = true; } }
    if (form_get(body, "api_key", v, sizeof(v))) { strncpy(m.api_key, v, sizeof(m.api_key) - 1); m.api_key[sizeof(m.api_key) - 1] = 0; m.valid = true; mdirty = true; }
    if (mdirty) settings_save_moonraker_slot(slot, &m);

    if (form_get(body, "delay", v, sizeof(v)) && v[0]) {
        int d = atoi(v);
        if (d >= 1 && d <= 60) settings_save_saver_delay(d);
    }

    /* 天气时钟配置（经纬度 + 城市显示名） */
    if (form_get(body, "wkey", v, sizeof(v))) settings_save_weather_key(v);
    if (form_get(body, "wcity", v, sizeof(v))) settings_save_weather_city(v);
    if (form_get(body, "wcityname", v, sizeof(v))) settings_save_weather_city_name(v);

    /* OTA 服务器地址（开机自动检测） */
    if (form_get(body, "otaurl", v, sizeof(v))) settings_save_ota_url(v);

    /* 3 秒后重启生效（WiFi/打印机配置变更需要重连） */
    esp_timer_handle_t t;
    if (esp_timer_create(&(esp_timer_create_args_t){ .callback = reboot_cb, .arg = NULL }, &t) == ESP_OK) {
        esp_timer_start_once(t, 3 * 1000 * 1000);
    }
    httpd_resp_sendstr(req, "OK, rebooting in 3s");
    ESP_LOGI(TAG, "config saved (slot %d), reboot in 3s", slot);
    return ESP_OK;
}

/* ---------------- 启动 ---------------- */

/* 仅开启 Web 服务（不碰热点）。长按息屏键 5s 开热点后由 bsp 调用 */
void web_config_server_start(void)
{
    if (s_server) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;      /* 4096 在手机访问大页面时栈溢出；16384 设备内存创建失败，取 8192 */
    cfg.max_uri_handlers = 8;
    cfg.send_wait_timeout = 30;
    cfg.recv_wait_timeout = 120;    /* 大固件上传时等待下一段数据的超时 */
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err == ESP_OK) {
        httpd_uri_t u;
        u.user_ctx = NULL;
        u.method = HTTP_GET;
        u.handler = handle_root;
        u.uri = "/";
        httpd_register_uri_handler(s_server, &u);
        u.handler = handle_status;
        u.uri = "/api/status";
        httpd_register_uri_handler(s_server, &u);
        u.method = HTTP_POST;
        u.handler = handle_config;
        u.uri = "/api/config";
        httpd_register_uri_handler(s_server, &u);
        u.handler = handle_ota;
        u.uri = "/api/ota";
        httpd_register_uri_handler(s_server, &u);
        ESP_LOGI(TAG, "web config server on :80");
    } else {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(err));
        s_server = NULL;
    }
}

void web_config_start(void)
{
    if (s_server) return;

    /* 仅在无 WiFi 配置的首次开机自动开热点引导配置；
       已配置 WiFi 时不开热点、不启 Web 服务——后台只由
       长按息屏键 5 秒手动开启热点后访问 192.168.4.1。 */
    wifi_conf_t wc;
    bool have_wifi = settings_load_wifi(&wc) && wc.valid && wc.ssid[0];
    if (!have_wifi) {
        bsp_wifi_start_ap(AP_SSID, AP_PASS);
        ESP_LOGW(TAG, "no wifi config -> SoftAP '%s' up, config page at http://192.168.4.1", AP_SSID);
        web_config_server_start();
    } else {
        ESP_LOGI(TAG, "wifi configured -> no web server (long-press sleep key 5s to open AP+config)");
    }
}