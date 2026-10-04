#pragma once
/*
 * printer_model 给 moonraker_client 的内部接口（不在 printer.h 公共契约里）。
 */
#ifdef __cplusplus
extern "C" {
#endif

/* 把一段 Moonraker status 字典的 JSON 文本合入模型（如订阅响应 result.status
 * 或 notify_status_update 的 params[0]）。
 * 必须在 LVGL 任务上下文执行（经 lv_async_call 投递）；json_heap 由本函数释放。 */
void printer_model_apply_status_json(char *json_heap);

/* Moonraker 连接状态变化（OFFLINE/READY）时通知模型重新评估打印机状态。
 * 同样在 LVGL 上下文执行。 */
void printer_model_set_online(int online);

/* 上报一条 GCode 响应行（notify_gcode_response，如 "!! Endstop not triggered"）。
 * LVGL 上下文执行；msg_heap 由本函数释放。
 * "!!" 行记为待 UI 提示的错误；全部行追加进控制台环形缓冲；
 * "x:open"/"y:TRIGGERED" 等行解析为限位状态（QUERY_ENDSTOP 回流）。 */
void printer_model_report_gcode_response(char *msg_heap);

/* objects.list 的结果数组（["extruder","gcode_macro FOO",...]）JSON 文本，
 * 用于构建宏/风扇/断料传感器清单与动态订阅项。LVGL 上下文执行，函数负责释放。 */
void printer_model_set_object_names(char *json_heap);

/* 上报一条 JSON-RPC 层错误（如点动未归位时 moonraker 直接回 RPC error，
 * 不走 gcode 响应行）。LVGL 上下文执行；msg_heap 由本函数释放。 */
void printer_model_report_rpc_error(char *msg_heap);

/* 更新应用层心跳 RTT（毫秒）。LVGL 上下文执行。 */
void printer_model_set_rtt(int ms);

#ifdef __cplusplus
}
#endif
