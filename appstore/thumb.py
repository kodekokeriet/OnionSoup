#!/usr/bin/env python3
"""Make a centre-cropped thumbnail for the App Store. Usage: thumb.py <in> <out.png> [width height]
The default 160x225 is a portrait cover; themes use 256x192 (4:3, like the screen)."""
import sys
from PIL import Image

W, H = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (160, 225)
im = Image.open(sys.argv[1]).convert("RGBA")
scale = max(W / im.width, H / im.height)
im = im.resize((max(W, round(im.width * scale)), max(H, round(im.height * scale))), Image.LANCZOS)
left, top = (im.width - W) // 2, (im.height - H) // 2
im.crop((left, top, left + W, top + H)).convert("RGB").save(sys.argv[2], optimize=True)
