#pragma once
/*
 * G-code 内嵌缩略图提取（PrusaSlicer/OrcaSlicer/Cura 的
 * "; thumbnail begin WxH len" 注释块，内容为 base64 PNG）。
 * 取扫描范围内面积最大的一块；多块只扫文件头/尾各一段，避免整读大文件。
 * 仅 Linux 上位机（BSP_HAS_LINUX_HOST）编译出实体，其余平台恒 false。
 */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gcode_name（相对 gcodes 根的路径）→ 本地完整路径。
 * 根目录：$KLIPPER_GCODES_DIR 优先，否则 $HOME/printer_data/gcodes。 */
bool gcode_thumbnail_resolve(const char *gcode_name, char *out, size_t cap);

/* 从 gcode 文件提取缩略图 PNG。成功返回 true：*png_out 为 malloc 的
 * PNG 字节流（调用方负责 free），*png_len 为其长度。 */
bool gcode_thumbnail_load(const char *gcode_path, unsigned char **png_out, size_t *png_len);

#ifdef __cplusplus
}
#endif
