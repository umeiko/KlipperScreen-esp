import urllib.request, os
from PIL import Image

styles = {
    "mengfo": ("https://aka.doubaocdn.com/s/n4xUKGNZ8T",
               "https://aka.doubaocdn.com/s/LhKvuAWMdz",
               "https://aka.doubaocdn.com/s/FfsoiL5PrB"),
    "real":   ("https://aka.doubaocdn.com/s/p6s3xiWHq1",
               "https://aka.doubaocdn.com/s/KhdgrrBpJR",
               "https://aka.doubaocdn.com/s/vktbZ3CRc1"),
    "paper":  ("https://aka.doubaocdn.com/s/vqLjL3TKZT",
               "https://aka.doubaocdn.com/s/qaJZnM642D",
               "https://aka.doubaocdn.com/s/VWzJNvj8D3"),
}

outdir = r"C:\Users\35978\Desktop"
for name, (b, f1, f2) in styles.items():
    frames = []
    for i, url in enumerate([b, f1, f2]):
        p = f"_{name}{i}.jpg"
        urllib.request.urlretrieve(url, p)
        frames.append(Image.open(p).convert("RGB"))
    # viewing gif
    view = [im.resize((640,480), Image.LANCZOS) for im in frames]
    seq = [view[0], view[1], view[2], view[1]]
    out = os.path.join(outdir, f"供奉3D打印机_{name}.gif")
    seq[0].save(out, save_all=True, append_images=seq[1:], duration=600, loop=0, optimize=True)
    print(name, os.path.getsize(out))
