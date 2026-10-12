from PIL import Image

frames = []
for i in range(3):
    im = Image.open(f"_paper{i}.jpg").convert("RGB").resize((320, 240), Image.LANCZOS)
    frames.append(im)

# RGB565 小端
def to_rgb565_bytes(im):
    px = im.load()
    out = bytearray()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y]
            rgb = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)   # RGB565（面板已配 RGB 序）
            out.append(rgb & 0xFF)
            out.append((rgb >> 8) & 0xFF)
    return out

with open("saver_frames.h", "w", encoding="utf-8") as f:
    f.write("#pragma once\n#include \"lvgl.h\"\n\n")
    f.write("/* 供奉3D打印机 屏保，扁平剪纸风，320x240 RGB565，3帧循环 */\n")
    for i, im in enumerate(frames):
        data = to_rgb565_bytes(im)
        f.write(f"static const uint16_t saver_frame_{i}_data[{len(data)//2}] = {{\n")
        for j in range(0, len(data), 2):
            v = data[j] | (data[j+1] << 8)
            f.write(f"0x{v:04X},")
            if (j//2 + 1) % 16 == 0:
                f.write("\n")
        f.write("};\n\n")
    f.write("static const lv_color_t *saver_frames[3] = {\n")
    for i in range(3):
        f.write(f"    (const lv_color_t *)saver_frame_{i}_data,\n")
    f.write("};\n")
print("done, sizes:", [len(to_rgb565_bytes(im)) for im in frames])
