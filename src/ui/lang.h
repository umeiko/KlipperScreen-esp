#pragma once
/*
 * UI 多语言：简体中文串作为 key，运行时按当前语言查表替换。
 * 翻译钩子内置在 theme_label/theme_button/titlebar_set/ui_toast/confirm_open
 * 等助手里；直接调 lv_label_set_text 的个别位置用 TR() 包一层。
 * 偏好存 klipperscreen.conf（language=zh|en|tw|fr|it），ui_app_create 启动时加载。
 *
 * 新增一门语言：
 *   1. ui_lang_t 尾部追加枚举值；langs[] 注册表加 {枚举, 配置代码, 母语显示名}
 *   2. lang.c 的 dict_entry_t 加一列译文，dict[] 每条补上
 *   3. 重跑 python tools/fontgen/gen_fonts.py（自动收录新字形）
 * 设置面板的语言下拉框按注册表动态生成，无需改面板代码。
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LANG_ZH = 0,  /* 简体中文（language=zh） */
    UI_LANG_EN,      /* English（language=en，默认） */
    UI_LANG_ZH_TW,   /* 繁體中文（language=tw） */
    UI_LANG_FR,      /* Français（language=fr） */
    UI_LANG_IT,      /* Italiano（language=it） */
    UI_LANG_COUNT
} ui_lang_t;

void ui_lang_set(ui_lang_t l);
ui_lang_t ui_lang_get(void);

/* 语言注册表 */
unsigned ui_lang_count(void);
const char *ui_lang_code(ui_lang_t l);          /* 配置文件用的代码："zh"/"en"/"tw"/"fr"/"it" */
const char *ui_lang_name(ui_lang_t l);          /* 下拉框显示名（各语言母语，不做翻译） */
ui_lang_t ui_lang_from_code(const char *code);  /* 未知/空代码回退 UI_LANG_EN */

/* 查表翻译；无匹配（数字/文件名/纯 ASCII 等）原样返回 */
const char *ui_tr(const char *zh);
#define TR(zh) ui_tr(zh)

/* 从 klipperscreen.conf 读语言并应用（须在构建任何 UI 前调用） */
void ui_lang_load(void);

#ifdef __cplusplus
}
#endif
