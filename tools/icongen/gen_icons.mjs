// SVG 图标 → LVGL C 数组。普通图标用 A8 供主题重染，品牌标志可保留真彩色。
// 用法：node tools/icongen/gen_icons.mjs [可选的输出名...]
// 新增图标：把 SVG 放进 src/ui/assets/svg/，在下面 ICONS 里加一行，重跑本脚本。
import { Resvg } from '@resvg/resvg-js';
import { readFileSync, writeFileSync, mkdirSync } from 'fs';
import { execFileSync } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const SVG_DIR = join(ROOT, 'src/ui/assets/svg');
const OUT_C = join(ROOT, 'src/ui/assets');
const OUT_PNG = join(ROOT, 'tmp/icongen');
const LVGLIMG = join(ROOT, 'third_party/lvgl/scripts/LVGLImage.py');
// LVGLImage.py 需要 pypng，装在 tools/icongen/.venv 里
const PY = join(ROOT, 'tools/icongen/.venv/Scripts/python.exe');

// [svg 名, 输出尺寸(px), C 变量名后缀, 可选 LVGL 色彩格式]
const ICONS = [
  ['heater',         28, 'heater'],        // 主菜单"温度"、温度面板
  ['heater',         56, 'heater_56'],     // 大屏(800x480)主菜单 2x 变体
  ['extruder',       16, 'nozzle_16'],     // 标题栏喷嘴图标
  ['extruder',       32, 'nozzle_32'],     // 温度面板喷嘴大卡
  ['bed',            16, 'bed_16'],        // 标题栏热床图标
  ['bed',            32, 'bed_32'],        // 温度面板热床大卡
  ['move',           28, 'move'],
  ['move',           56, 'move_56'],
  ['extrude',        28, 'extrude'],
  ['extrude',        56, 'extrude_56'],
  ['files',          28, 'files'],
  ['files',          56, 'files_56'],
  ['printer',        28, 'printer'],
  ['printer',        56, 'printer_56'],
  ['settings',       28, 'settings'],
  ['settings',       56, 'settings_56'],
  ['wifi_excellent', 18, 'wifi_4'],
  ['wifi_good',      18, 'wifi_3'],
  ['wifi_fair',      18, 'wifi_2'],
  ['wifi_weak',      18, 'wifi_1'],
  ['link_off',       16, 'link_off'],      // 主菜单状态卡：Moonraker 断连
  ['link_off',       32, 'link_off_32'],   // 大屏 2x 变体
  ['link',           16, 'link'],          // 主菜单状态卡：已连接
  ['link',           32, 'link_32'],
  ['alert_circle',   16, 'alert_circle'],  // 主菜单状态卡：Klipper 异常
  ['alert_circle',   32, 'alert_circle_32'],
  ['web',            16, 'globe_16'],     // 设置-语言行小地球
  ['web',            32, 'globe_32'],     // 大屏 2x 变体
  ['toolchanger',    16, 'swap_16'],      // Moonraker-切换打印机行：双向箭头
  ['toolchanger',    32, 'swap_32'],      // 大屏 2x 变体
  ['klipper_logo',   56, 'klipper_logo_56',  'RGB565A8'], // 官方红灰双色
  ['klipper_logo',  112, 'klipper_logo_112', 'RGB565A8'], // 大屏 2x 变体
  ['bambu_logo',     56, 'bambu_logo_56'],    // 机器模式；槽位页缩至约 32px
  ['bambu_logo',    112, 'bambu_logo_112'],   // 大屏 2x 变体
];

mkdirSync(OUT_PNG, { recursive: true });
const requested = new Set(process.argv.slice(2));

for (const [svg, size, name, colorFormat = 'A8'] of ICONS) {
  if (requested.size && !requested.has(name)) continue;
  const r = new Resvg(readFileSync(join(SVG_DIR, `${svg}.svg`)), {
    fitTo: { mode: 'width', value: size },
    // 透明背景；A8 图标只取 alpha，真彩品牌标志保留 SVG 颜色。
  });
  const png = join(OUT_PNG, `img_${name}.png`);
  writeFileSync(png, r.render().asPng());

  // 注意：LVGLImage.py 的 -o 是输出目录，文件名取输入 PNG 名
  execFileSync(PY, [LVGLIMG, '--ofmt', 'C', '--cf', colorFormat, '-o', OUT_C, png],
               { cwd: ROOT, stdio: 'inherit' });

  // 生成的 include 条件块在 ESP-IDF(Kconfig 配置 LVGL) 下会落到 "lvgl/lvgl.h"，
  // 而 IDF 组件没有 lvgl/ 子目录 → 编译失败。统一改成直接 #include "lvgl.h"。
  const cFile = join(OUT_C, `img_${name}.c`);
  const src = readFileSync(cFile, 'utf8').replace(
    /#if defined\(LV_LVGL_H_INCLUDE_SIMPLE\)[\s\S]*?#endif/,
    '#include "lvgl.h"').replace(/\s+$/, '\n');
  writeFileSync(cFile, src);
  console.log(`img_${name}.c  (${size}px)`);
}
console.log('done');
