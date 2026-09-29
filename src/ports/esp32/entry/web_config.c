/*
 * Web 配置后台：esp_http_server 提供手机配置页
 *  - GET  /                 配置页面（HTML）
 *  - GET  /api/status       当前配置 JSON（WiFi + 全 6 槽位打印机 + 屏保延时）
 *  - POST /api/config       保存 WiFi/打印机（slot 参数）/屏保延时，3 秒后自动重启生效
 *  - POST /api/wallpaper    上传屏保壁纸（?idx=0..2 原始 JPEG body，解码缩放到 320x240 RGB565 存 flash）
 *  - POST /api/wallpaper?clear=1   删除全部壁纸，恢复内置供奉动图
 * 启动时若无 WiFi 配置，自动开启 SoftAP 热点（KlipperScreen-AP / 12345678），
 * 手机连接热点后访问 http://192.168.4.1 完成首次配置。
 * 所有耗时操作（解码/写 flash）都在 httpd 任务内执行，不影响 LVGL UI 任务。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "app_settings.h"
#include "bsp_wifi.h"

#define TAG "webcfg"

#define WALL_DIR   "/littlefs/wallpaper"
#define WALL_W     320
#define WALL_H     240
#define WALL_MAX   3
#define IMG_MAX_BYTES (4 * 1024 * 1024)  /* 上传图片最大 4MB */
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
    "<button onclick='saveCfg()'>保存并重启</button>"
    "<h2>屏保壁纸</h2>"
    "<div class='tip'>上传 1~3 张图（JPG），屏保按帧循环播放；不够 3 张时用内置动图补足；恢复默认动图请点下方按钮。</div>"
    "<label>第 1 帧</label><input id='f0' type='file' accept='image/jpeg,.jpg,.jpeg'>"
    "<label>第 2 帧（可选）</label><input id='f1' type='file' accept='image/jpeg,.jpg,.jpeg'>"
    "<label>第 3 帧（可选）</label><input id='f2' type='file' accept='image/jpeg,.jpg,.jpeg'>"
    "<button onclick='up(0)'>上传第 1 帧</button>"
    "<button class='sec' onclick='up(1)'>上传第 2 帧</button>"
    "<button class='sec' onclick='up(2)'>上传第 3 帧</button>"
    "<button class='danger' onclick='clearWall()'>恢复默认动图（删除壁纸）</button>"
    "<div id='status'></div>";

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
    "try{var r=await fetch('/api/config',{method:'POST',body:b});"
    "show(await r.text(),r.ok);if(r.ok)show('保存成功，3 秒后自动重启…',true)}"
    "catch(e){show('网络错误: '+e,false)}}"
    "async function up(i){var f=document.getElementById('f'+i);if(!f.files[0]){show('请先选择第 '+(i+1)+' 张图片',false);return}"
    "try{var r=await fetch('/api/wallpaper?idx='+i,{method:'POST',body:f.files[0]});"
    "show(await r.text(),r.ok)}catch(e){show('上传失败: '+e,false)}}"
    "async function clearWall(){if(!confirm('删除全部壁纸并恢复默认供奉动图？'))return;"
    "try{var r=await fetch('/api/wallpaper?clear=1',{method:'POST'});show(await r.text(),r.ok)}catch(e){show('网络错误: '+e,false)}}"
    "async function load(){try{var r=await fetch('/api/status');J=await r.json();"
    "document.getElementById('ssid').value=J.ssid||'';"
    "document.getElementById('pass').value=J.pass||'';"
    "document.getElementById('delay').value=J.delay||5;"
    "SLOT=(J.active||0);fill();"
    "document.getElementById('slotrow').children[SLOT].style.background='#2d9cdb'}catch(e){}}"
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

/* 最近邻缩放 RGB565 到 320x240 */
static void scale_nearest(const uint16_t *src, int sw, int sh, uint16_t *dst)
{
    for (int y = 0; y < WALL_H; y++) {
        int sy = (y * sh) / WALL_H;
        const uint16_t *row = src + (size_t)sy * sw;
        for (int x = 0; x < WALL_W; x++) {
            dst[y * WALL_W + x] = row[(x * sw) / WALL_W];
        }
    }
}

/* ---------------- HTTP 处理 ---------------- */

static void log_req(httpd_req_t *req)
{
    ESP_LOGI(TAG, "REQ %s %s", req->method == HTTP_GET ? "GET" : "POST", req->uri);
}
static esp_err_t handle_root(httpd_req_t *req)
{
    log_req(req);
    /* 三段合成一段发送，交由 httpd 自动处理长度 */
    static char s_page[8192];
    static bool s_built = false;
    if (!s_built) {
        snprintf(s_page, sizeof(s_page), "%s%s%s", PAGE_HEAD, PAGE_FORM, PAGE_JS);
        s_built = true;
    }
    ESP_LOGI(TAG, "PAGE total=%d", (int)strlen(s_page));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr(req, s_page);
    ESP_LOGI(TAG, "PAGE send: %s", esp_err_to_name(e));
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    log_req(req);
    wifi_conf_t w;
    char buf[2048];
    settings_load_wifi(&w);
    int delay = settings_load_saver_delay();
    int active = settings_load_active_printer();
    char esc_ssid[64], esc_pass[128];
    json_escape(esc_ssid, sizeof(esc_ssid), w.ssid);
    json_escape(esc_pass, sizeof(esc_pass), w.pass);
    char *p = buf;
    int n = snprintf(p, sizeof(buf) - (size_t)(p - buf),
                     "{\"ssid\":\"%s\",\"pass\":\"%s\",\"delay\":%d,\"active\":%d,\"slots\":[",
                     esc_ssid, esc_pass, delay, active);
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

    /* 3 秒后重启生效（WiFi/打印机配置变更需要重连） */
    esp_timer_handle_t t;
    if (esp_timer_create(&(esp_timer_create_args_t){ .callback = reboot_cb, .arg = NULL }, &t) == ESP_OK) {
        esp_timer_start_once(t, 3 * 1000 * 1000);
    }
    httpd_resp_sendstr(req, "OK, rebooting in 3s");
    ESP_LOGI(TAG, "config saved (slot %d), reboot in 3s", slot);
    return ESP_OK;
}

static esp_err_t handle_wallpaper(httpd_req_t *req)
{
    log_req(req);
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "clear", val, sizeof(val)) == ESP_OK && strcmp(val, "1") == 0) {
            for (int i = 0; i < WALL_MAX; i++) {
                char path[64];
                snprintf(path, sizeof(path), WALL_DIR "/wall_%d.raw", i);
                remove(path);
            }
            httpd_resp_sendstr(req, "壁纸已清除，恢复内置动图");
            return ESP_OK;
        }
    }

    int idx = 0;
    char idxs[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "idx", idxs, sizeof(idxs)) == ESP_OK) {
        idx = atoi(idxs);
    }
    if (idx < 0 || idx >= WALL_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
        return ESP_OK;
    }

    /* 读 body 到 PSRAM */
    size_t cap = IMG_MAX_BYTES;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    size_t got = 0;
    int r;
    while ((r = httpd_req_recv(req, body + got, (int)(cap - got))) > 0) {
        got += (size_t)r;
        if (got >= cap) break;
    }
    if (got < 4) { heap_caps_free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_OK; }

    int sw, sh;
    esp_jpeg_image_cfg_t icfg = { 0 };
    icfg.indata = (uint8_t *)body;
    icfg.indata_size = (uint32_t)got;
    icfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&icfg, &info) != ESP_OK || info.width == 0 || info.height == 0 ||
        info.width > 1024 || info.height > 1024) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "图片过大或不是 JPG（限制 ≤1024px）");
        return ESP_OK;
    }
    sw = info.width;
    sh = info.height;

    /* 解码 RGB565（整图到 PSRAM） */
    size_t outcap = (size_t)sw * sh * 2;
    uint8_t *out = heap_caps_malloc(outcap, MALLOC_CAP_SPIRAM);
    if (!out) { heap_caps_free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }

    icfg.outbuf = out;
    icfg.outbuf_size = (uint32_t)outcap;
    esp_jpeg_image_output_t res;
    esp_err_t err = esp_jpeg_decode(&icfg, &res);
    if (err != ESP_OK || res.output_len == 0) {
        heap_caps_free(body);
        heap_caps_free(out);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JPEG 解码失败");
        return ESP_OK;
    }
    sw = res.width;
    sh = res.height;

    /* 缩放 320x240 并写文件 */
    uint16_t *scaled = heap_caps_malloc(WALL_W * WALL_H * 2, MALLOC_CAP_SPIRAM);
    if (!scaled) { heap_caps_free(body); heap_caps_free(out); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    scale_nearest((const uint16_t *)out, sw, sh, scaled);

    mkdir(WALL_DIR, 0777);
    char path[64];
    snprintf(path, sizeof(path), WALL_DIR "/wall_%d.raw", idx);
    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write fail");
    } else {
        size_t n = fwrite(scaled, 2, WALL_W * WALL_H, f);
        fclose(f);
        char msg[96];
        snprintf(msg, sizeof(msg), "第 %d 帧已保存（%u 字节），下次屏保生效", idx + 1, (unsigned)(n * 2));
        httpd_resp_sendstr(req, msg);
        ESP_LOGI(TAG, "wallpaper %d saved (%dx%d -> 320x240)", idx, sw, sh);
    }
    heap_caps_free(body);
    heap_caps_free(out);
    heap_caps_free(scaled);
    return ESP_OK;
}

/* ---------------- 启动 ---------------- */

void web_config_start(void)
{
    if (s_server) return;

    /* 有 WiFi 配置则不开热点（保存重启后热点关闭）；仅首次无配置时开热点引导配置。
       运行中长按息屏键 5 秒可手动开启热点进后台。 */
    wifi_conf_t wc;
    bool have_wifi = settings_load_wifi(&wc) && wc.valid && wc.ssid[0];
    if (!have_wifi) {
        bsp_wifi_start_ap(AP_SSID, AP_PASS);
        ESP_LOGW(TAG, "no wifi config -> SoftAP '%s' up, config page at http://192.168.4.1", AP_SSID);
    } else {
        ESP_LOGI(TAG, "wifi configured -> SoftAP off (long-press sleep key 5s to reopen)");
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    if (httpd_start(&s_server, &cfg) == ESP_OK) {
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
        u.handler = handle_wallpaper;
        u.uri = "/api/wallpaper";
        httpd_register_uri_handler(s_server, &u);
        ESP_LOGI(TAG, "web config server on :80");
    } else {
        ESP_LOGE(TAG, "httpd start failed");
    }
}