# -*- coding: utf-8 -*-
from PIL import Image
import pillow_heif

pillow_heif.register_heif_opener()

src = r"C:\Users\goldg\Pictures\相机\IMG_20260911_093426.HEIC"
dst = r"D:\mimo\车库门自动化\IMG_20260911_093426.jpg"

img = Image.open(src)
img = img.convert("RGB")
img.save(dst, quality=92)
print("size:", img.size, "->", dst)
