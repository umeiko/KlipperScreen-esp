from PIL import Image
import os

frames = []
for f in ["base.jpg", "frame1.jpg", "frame2.jpg"]:
    im = Image.open(f).convert("RGB")
    frames.append(im)

# 查看用 GIF（中等尺寸）
view = [im.resize((640, 480), Image.LANCZOS) for im in frames]
view[0].save("saver_view.gif", save_all=True, append_images=view[1:],
             duration=600, loop=0, optimize=True)

# 屏幕用 GIF（320x240，顺序 base->f1->f2->f1 来回）
dev = [im.resize((320, 240), Image.LANCZOS) for im in frames]
seq = [dev[0], dev[1], dev[2], dev[1]]
seq[0].save("saver_320x240.gif", save_all=True, append_images=seq[1:],
            duration=500, loop=0, optimize=True)

for f in ["saver_view.gif", "saver_320x240.gif"]:
    print(f, os.path.getsize(f))
