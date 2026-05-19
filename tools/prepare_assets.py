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

    # Fit logo to 72% of icon size, preserving aspect ratio, centered on BG.
    # 72% keeps content clear of launcher mask shapes (circle/squircle clip).
    max_logo = int(sz * 0.72)
    src_w, src_h = logo.size
    scale    = min(max_logo / src_w, max_logo / src_h)
    logo_w   = int(src_w * scale)
    logo_h   = int(src_h * scale)
    off_x    = (sz - logo_w) // 2
    off_y    = (sz - logo_h) // 2
    base     = Image.new("RGBA", (sz, sz), BG_COLOR)
    scaled   = logo.resize((logo_w, logo_h), Image.LANCZOS)
    base.paste(scaled, (off_x, off_y), scaled)
    out      = base.convert("RGB")

    out.save(os.path.join(out_dir, "ic_launcher.png"))
    out.save(os.path.join(out_dir, "ic_launcher_round.png"))
    print(f"  → {folder}/ic_launcher{{,_round}}.png  ({sz}×{sz}, logo {logo_w}×{logo_h})")

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
    # Safe zone = inner 66dp of 108dp canvas (61.1%). Use 56% to stay well clear.
    # Preserve aspect ratio so the logo isn't distorted.
    max_logo = int(canvas * 0.56)
    src_w, src_h = logo.size
    scale    = min(max_logo / src_w, max_logo / src_h)
    logo_w   = int(src_w * scale)
    logo_h   = int(src_h * scale)
    off_x    = (canvas - logo_w) // 2
    off_y    = (canvas - logo_h) // 2
    fg       = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    scaled   = logo.resize((logo_w, logo_h), Image.LANCZOS)
    fg.paste(scaled, (off_x, off_y), scaled)
    fg.save(os.path.join(out_dir, "ic_launcher_foreground.png"))
    print(f"  → {folder}/ic_launcher_foreground.png  ({canvas}×{canvas} canvas, logo {logo_w}×{logo_h})")

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
