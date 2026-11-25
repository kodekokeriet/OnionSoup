#!/usr/bin/env python3
"""Make a 160x225 cover thumbnail (centre-cropped) for the App Store. Usage: thumb.py <in> <out.png>"""
import sys
from PIL import Image

W, H = 160, 225
im = Image.open(sys.argv[1]).convert("RGBA")
scale = max(W / im.width, H / im.height)
im = im.resize((max(W, round(im.width * scale)), max(H, round(im.height * scale))), Image.LANCZOS)
left, top = (im.width - W) // 2, (im.height - H) // 2
im.crop((left, top, left + W, top + H)).convert("RGB").save(sys.argv[2], optimize=True)
