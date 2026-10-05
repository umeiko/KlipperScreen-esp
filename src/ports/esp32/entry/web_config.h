#pragma once
/* Web 后台配置：手机浏览器访问 http://<ESP32-IP>/ 修改 WiFi/打印机配置、上传屏保壁纸 */
#ifdef __cplusplus
extern "C" {
#endif

void web_config_start(void);
void web_config_server_start(void);   /* 仅启动 :80 Web 服务（开热点后调用） */

#ifdef __cplusplus
}
#endif
