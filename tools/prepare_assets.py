#!/usr/bin/env python3
"""
PocketDisplay asset preparation script.

Generates:
  windows/resources/icon.ico               16/32/48/64/128/256 px  (from logo-icon.png)
  mipmap-*/ic_launcher.png                 48/72/96/144/192 px  (#FFF7EF bg)
  mipmap-*/ic_launcher_round.png           same
  mipmap-*/ic_launcher_foreground.png      transparent bg, 108dp adaptive canvas
  drawable/logo_icon.png                   copy of logo-icon.png
  drawable/logo_primary.png                copy of logo-primary.png
  font/anton_regular.ttf                   Anton 400
  font/space_grotesk_medium.ttf            Space Grotesk 500
  font/space_grotesk_bold.ttf              Space Grotesk 700
"""

import os, sys, shutil
from PIL import Image

BASE        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS      = os.path.join(BASE, "assets")
ANDROID_RES = os.path.join(BASE, "android", "app", "src", "main", "res")
WIN_RES     = os.path.join(BASE, "windows", "resources")

os.makedirs(WIN_RES, exist_ok=True)

icon_path = os.path.join(ASSETS, "logo-icon.png")
if not os.path.exists(icon_path):
    print(f"ERROR: {icon_path} not found", file=sys.stderr)
    sys.exit(1)

logo = Image.open(icon_path).convert("RGBA")
print(f"Loaded logo-icon.png  {logo.size[0]}x{logo.size[1]} px")

# ── Windows icon.ico ─────────────────────────────────────────────────────────
ICO_SIZES = [16, 32, 48, 64, 128, 256]
ico_path  = os.path.join(WIN_RES, "icon.ico")
imgs = [logo.resize((s, s), Image.LANCZOS) for s in ICO_SIZES]
imgs[0].save(ico_path, format="ICO", sizes=[(s, s) for s in ICO_SIZES],
             append_images=imgs[1:])
print(f"  → {ico_path}  ({', '.join(str(s) for s in ICO_SIZES)} px)")

# ── Android regular launcher icons ───────────────────────────────────────────
LAUNCHER = [
    ("mipmap-mdpi",    48),
    ("mipmap-hdpi",    72),
    ("mipmap-xhdpi",   96),
    ("mipmap-xxhdpi",  144),
    ("mipmap-xxxhdpi", 192),
]

BG_COLOR = (0xFF, 0xF7, 0xEF, 255)   # #FFF7EF brand background

for folder, sz in LAUNCHER:
    out_dir = os.path.join(ANDROID_RES, folder)
    os.makedirs(out_dir, exist_ok=True)

    pad      = max(4, sz // 12)
    inner_sz = sz - pad * 2
    base     = Image.new("RGBA", (sz, sz), BG_COLOR)
    scaled   = logo.resize((inner_sz, inner_sz), Image.LANCZOS)
    base.paste(scaled, (pad, pad), scaled)
    out      = base.convert("RGB")

    out.save(os.path.join(out_dir, "ic_launcher.png"))
    out.save(os.path.join(out_dir, "ic_launcher_round.png"))
    print(f"  → {folder}/ic_launcher{{,_round}}.png  ({sz}×{sz})")

# ── Adaptive icon foreground PNGs ─────────────────────────────────────────────
ADAPTIVE = [
    ("mipmap-mdpi",    108),
    ("mipmap-hdpi",    162),
    ("mipmap-xhdpi",   216),
    ("mipmap-xxhdpi",  324),
    ("mipmap-xxxhdpi", 432),
]

for folder, canvas in ADAPTIVE:
    out_dir  = os.path.join(ANDROID_RES, folder)
    os.makedirs(out_dir, exist_ok=True)
    logo_sz  = int(canvas * 0.80)
    offset   = (canvas - logo_sz) // 2
    fg       = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    scaled   = logo.resize((logo_sz, logo_sz), Image.LANCZOS)
    fg.paste(scaled, (offset, offset), scaled)
    fg.save(os.path.join(out_dir, "ic_launcher_foreground.png"))
    print(f"  → {folder}/ic_launcher_foreground.png  ({canvas}×{canvas} canvas)")

# ── Android drawable PNGs ─────────────────────────────────────────────────────
drawable_dir = os.path.join(ANDROID_RES, "drawable")
os.makedirs(drawable_dir, exist_ok=True)
shutil.copy2(os.path.join(ASSETS, "logo-icon.png"),    os.path.join(drawable_dir, "logo_icon.png"))
shutil.copy2(os.path.join(ASSETS, "logo-primary.png"), os.path.join(drawable_dir, "logo_primary.png"))
print(f"  → drawable/logo_icon.png, logo_primary.png")

# ── Android font resources ────────────────────────────────────────────────────
font_dir = os.path.join(ANDROID_RES, "font")
os.makedirs(font_dir, exist_ok=True)
for src_name, dst_name in [
    ("Anton-Regular.ttf",       "anton_regular.ttf"),
    ("SpaceGrotesk-Medium.ttf", "space_grotesk_medium.ttf"),
    ("SpaceGrotesk-Bold.ttf",   "space_grotesk_bold.ttf"),
]:
    src = os.path.join(ASSETS, src_name)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(font_dir, dst_name))
        print(f"  → font/{dst_name}")
    else:
        print(f"  WARN: {src_name} not found, skipping", file=sys.stderr)

print("\nAll assets generated successfully.")
