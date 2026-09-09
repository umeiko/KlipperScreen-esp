# LVGL v9 API 防错笔记

> 本项目锁 LVGL v9.3。v8→v9 有大量破坏性改名/签名变更，**写任何 LVGL 调用前先在
> `third_party/lvgl/src/` 的对应头文件里 grep 核对签名**，不要凭 v8 记忆写。
> 本文件持续记录已核对过的正确用法与踩过的坑。

## v8 → v9 改名对照（已踩过/高频）

| v8 记忆 | v9.3 正确写法 | 头文件 |
|---|---|---|
| `lv_scr_load` / `lv_scr_load_anim` | `lv_screen_load` / `lv_screen_load_anim` | core/lv_obj.h（screen 相关在 lv_display.h? 核对：lv_obj.h） |
| `lv_disp_*` / `lv_disp_t` | `lv_display_*` / `lv_display_t` | display/lv_display.h |
| `lv_img_create` / `lv_img_set_src` | `lv_image_create` / `lv_image_set_src` | widgets/image/lv_image.h |
| `lv_btn_create` | `lv_button_create` | widgets/button/lv_button.h |
| `lv_obj_del` | `lv_obj_delete` | core/lv_obj.h |
| `LV_STYLE_TRANSFORM_ZOOM` | `LV_STYLE_TRANSFORM_SCALE_X` + `_Y`（拆成两个属性） | misc/lv_style.h |
| `lv_disp_draw_buf` + `lv_disp_drv_register` | `lv_display_create` + `lv_display_set_buffers` + `lv_display_set_flush_cb` | display/lv_display.h |
| `lv_indev_drv_register` | `lv_indev_create` + `lv_indev_set_read_cb` | indev/lv_indev.h |
| 动画完成回调里 `lv_anim_get_var(a)` | **没有此函数**，直接读结构体字段 `a->var`（`void *`） | misc/lv_anim.h |
| `lv_disp_get_scr_act` | `lv_screen_active()` | display/lv_display.h |

## 签名变更（已踩过）

- `lv_style_transition_dsc_init(tr, props, path_cb, time, delay, user_data)` —— **6 个参数**（v8 是 5 个，多 user_data）。属性表以 `0` 结尾（不再用 `LV_STYLE_PROP_INV`）。
- 按压缩放反馈标准写法：
  ```c
  static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y, 0};
  static lv_style_transition_dsc_t tr;
  lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 120, 0, NULL);
  lv_obj_set_style_transform_scale_x(btn, 240, LV_STATE_PRESSED);  /* 256=100% */
  lv_obj_set_style_transform_scale_y(btn, 240, LV_STATE_PRESSED);
  lv_obj_set_style_transition(btn, &tr, LV_STATE_PRESSED);
  ```

## 动画系统（misc/lv_anim.h）

- 流程：`lv_anim_init` → `set_var/set_values/set_duration/set_path_cb/set_exec_cb` → `lv_anim_start`。
- 缓动路径：`lv_anim_path_ease_in/out/in_out`、`lv_anim_path_overshoot`、`lv_anim_path_bounce`、`lv_anim_path_linear`。
- "停顿 N 毫秒后做某事"：用一个 0→1、时长 1ms 的占位动画，设 `lv_anim_set_delay` + `lv_anim_set_completed_cb`。
- 注意：动画回调里拿对象用 `a->var`；`lv_anim_t` 是公开结构体。

## 快照/截图（others/snapshot/lv_snapshot.h）

- `LV_USE_SNAPSHOT 1`（默认 0）。
- `lv_draw_buf_t *lv_snapshot_take(lv_obj_t *obj, lv_color_format_t cf)`；用完 `lv_draw_buf_destroy`。
- 数据在 `snap->data`，宽/高/行字节数在 `snap->header.{w,h,stride}`（**stride 可能 ≠ w×bpp，遍历要用 stride**）。
- **坑**：快照缓冲走 `lv_malloc`；默认内置内存池 `LV_MEM_SIZE` 只有 64KB，放不下 320×240×2B=150KB → 返回 NULL。
  桌面端设 `LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB`；ESP32 端同理走系统堆（Kconfig 默认即 stdlib）。

## lv_conf.h 注意

- 自定义 lv_conf.h **必须** `#define LV_CONF_H`（用 `#pragma once` 不算），否则 lv_conf_internal.h 认为包含失败、全部回落默认值，只给一行 `#pragma message` 不报错——极难排查。
- desktop 端用 `LV_CONF_INCLUDE_SIMPLE` + include 路径方式引入（见 desktop/CMakeLists.txt）。

## SDL2 后端（lvgl/src/drivers/sdl/）

- `lv_sdl_window_create(hor, ver)` → display；`lv_sdl_mouse_create()` → 鼠标 pointer indev。
- `lv_sdl_mousewheel_create()` → encoder indev；滚轮产生 `enc_diff`，中键产生按下/抬起。两者可同时创建。
- `lv_sdl_window_set_zoom(disp, 2)` 放大窗口便于查看；`lv_sdl_window_set_title`。
- 事件泵在 `lv_timer_handler()` 内部处理，主循环 `lv_timer_handler(); SDL_Delay(5);` 即可。

## 字体与方框（已踩过）

- **任何直接 `lv_label_create` 的 label 都必须显式设字体**。不设则落到 `LV_FONT_DEFAULT`（montserrat_14），
  不含 CJK 与 LV_SYMBOL 字形 → 中文和图标全部变方框。统一走 `theme_label()` / `theme_button()`。
- **图标（LV_SYMBOL_*）必须用 `THEME_FONT_ICON`（montserrat_16）**，CJK 子集字体里没有 FontAwesome 私用区字形；
  反过来中文必须用 CJK 字体。图标和中文混排时拆成两个 label。
- CJK 字体是子集化的：**UI 新增字符串后必须重跑 `python tools/fontgen/gen_fonts.py`**（自动扫描字符串字面量）。

## 核对方便签

```bash
# 批量核对签名（写代码前跑一次）
grep -n "函数名" third_party/lvgl/src/**/*.h
```
