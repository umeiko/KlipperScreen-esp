#pragma once
/*
 * 面板管理器：面板注册表 + 导航栈 + 转场。
 * 对齐 KlipperScreen：面板懒加载缓存复用；打开即全量回放（on_show）；
 * 仅栈顶面板接收数据节拍（on_tick）。
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;                 /* "main" / "job_status" / ... */
    const char *title;                /* 标题栏文字 */
    lv_obj_t *scr;                    /* 缓存的屏幕对象（懒加载） */
    lv_obj_t *(*create)(void);        /* 首次构建，返回 screen 根对象 */
    void (*on_show)(void);            /* 每次显示时调用（全量刷新数据） */
    void (*on_tick)(void);            /* 数据节拍（仅栈顶面板收到） */
    int hide_temps;                   /* 标题栏不显示右侧温度（标题长的面板置 1） */
} panel_def_t;

void panel_mgr_init(void);
void panel_mgr_open(const char *name);   /* 入栈 + 左滑转场 */
void panel_mgr_back(void);               /* 出栈 + 右滑转场 */
void panel_mgr_home(void);
void panel_mgr_nav_refresh(void);   /* 动态列表重建后重建焦点组 */               /* 直接回主面板（清空导航栈） */
int  panel_mgr_depth(void);
const char *panel_mgr_current(void);
void panel_mgr_tick(void);               /* mock/数据层节拍入口 */

#ifdef __cplusplus
}
#endif
