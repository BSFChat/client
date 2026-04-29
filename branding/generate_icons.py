#!/usr/bin/env python3
"""Generate the BSFChat brand icon at every size the desktop bundles
(macOS .icns, Windows .ico, Linux .png) need from a single vector
description. Mirrors the Android adaptive icon — accent-teal
background, dark speech bubble with a thick teal "B" inside —
so the brand is consistent everywhere.

Run from the repo root:
    python3 client/branding/generate_icons.py

Output goes into `client/branding/`:
    BSFChat-1024.png       master raster (used by Linux + sources)
    BSFChat.iconset/       intermediate iconset (input to iconutil)
    BSFChat.icns           macOS bundle icon
    BSFChat.ico            Windows app+installer icon
"""
from PIL import Image, ImageDraw
import os
import subprocess
import sys

# ── Brand palette (matches Theme.qml + Android adaptive icon) ──
ACCENT = (124, 226, 201, 255)   # #7CE2C9 — same teal as the splash
DARK   = (12, 24, 33, 255)      # #0C1821 — speech bubble fill

HERE = os.path.dirname(os.path.abspath(__file__))


def draw_logo(canvas: int) -> Image.Image:
    """Render the BSFChat mark to a `canvas`×`canvas` RGBA image.

    Geometry mirrors `android/res/drawable/ic_launcher_foreground.xml`
    drawn over `ic_launcher_background.xml`'s solid accent fill —
    the speech-bubble outline lives in the inner 72/108 safe area,
    here scaled to whatever canvas size we're targeting.
    """
    img = Image.new("RGBA", (canvas, canvas), ACCENT)
    d = ImageDraw.Draw(img)

    # Coordinate system normalised to a 108-unit grid then scaled.
    s = canvas / 108.0
    def px(v: float) -> float:
        return v * s

    # Speech bubble (rounded rect with a tail). Drawn as a rounded
    # rectangle for the body; the tail is a triangle below.
    body = [px(22), px(30), px(86), px(76)]
    radius = px(10)
    d.rounded_rectangle(body, radius=radius, fill=DARK)
    # Tail: small triangle hanging off the bottom-left of the body.
    tail = [(px(38), px(76)), (px(48), px(76)), (px(38), px(84))]
    d.polygon(tail, fill=DARK)

    # Bold "B" glyph centred in the bubble. We draw it from
    # rectangles + arcs because Pillow's text renderer would need
    # a font face we can't bundle.
    stroke = px(5)
    left   = px(44)
    right  = px(60)
    top    = px(42)
    mid    = px(54)
    bot    = px(66)

    # Vertical spine of the B.
    d.rectangle([left, top, left + stroke, bot], fill=ACCENT)

    # Top + bottom horizontals.
    d.rectangle([left, top, right + stroke / 2, top + stroke], fill=ACCENT)
    d.rectangle([left, mid - stroke / 2, right + stroke / 2, mid + stroke / 2],
                fill=ACCENT)
    d.rectangle([left, bot - stroke, right + stroke / 2, bot], fill=ACCENT)

    # Two right-side lobes — half-circles that round the B's bumps.
    # Pillow's pieslice draws the segment and we then erase the
    # interior with the bubble-fill colour to make the B's loops
    # hollow, mirroring the Android stroke version.
    upper_box = [right - stroke, top, right + stroke * 2, mid + stroke / 2]
    d.pieslice(upper_box, start=-90, end=90, fill=ACCENT)
    inner_upper = [
        upper_box[0] + stroke, upper_box[1] + stroke,
        upper_box[2] - stroke, upper_box[3] - stroke,
    ]
    d.pieslice(inner_upper, start=-90, end=90, fill=DARK)

    lower_box = [right - stroke, mid - stroke / 2,
                 right + stroke * 2, bot]
    d.pieslice(lower_box, start=-90, end=90, fill=ACCENT)
    inner_lower = [
        lower_box[0] + stroke, lower_box[1] + stroke,
        lower_box[2] - stroke, lower_box[3] - stroke,
    ]
    d.pieslice(inner_lower, start=-90, end=90, fill=DARK)

    return img


def build_macos_icns():
    """macOS Iconset → .icns via the system iconutil tool.
    The .iconset must contain ten specific filenames at 1× and 2×
    densities (16 → 1024 px); iconutil refuses inputs that don't
    match the convention exactly."""
    iconset = os.path.join(HERE, "BSFChat.iconset")
    os.makedirs(iconset, exist_ok=True)

    # (filename, pixel size)
    spec = [
        ("icon_16x16.png", 16),
        ("icon_16x16@2x.png", 32),
        ("icon_32x32.png", 32),
        ("icon_32x32@2x.png", 64),
        ("icon_128x128.png", 128),
        ("icon_128x128@2x.png", 256),
        ("icon_256x256.png", 256),
        ("icon_256x256@2x.png", 512),
        ("icon_512x512.png", 512),
        ("icon_512x512@2x.png", 1024),
    ]
    for name, size in spec:
        out = draw_logo(size)
        out.save(os.path.join(iconset, name), format="PNG")

    icns = os.path.join(HERE, "BSFChat.icns")
    subprocess.run(
        ["iconutil", "-c", "icns", "-o", icns, iconset],
        check=True,
    )
    print(f"  → {icns}")


def build_windows_ico():
    """Windows .ico is a multi-image container. Pillow's ICO save
    accepts a single image plus a list of sizes; it generates each
    size internally with high-quality resampling. Using our 1024
    master ensures every embedded resolution looks crisp."""
    ico = os.path.join(HERE, "BSFChat.ico")
    master = draw_logo(1024)
    master.save(
        ico,
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
               (64, 64), (128, 128), (256, 256)],
    )
    print(f"  → {ico}")


def build_linux_png():
    """Single 512×512 PNG referenced from the .desktop file
    (Linux's display servers all support PNG hicolor icons)."""
    out = os.path.join(HERE, "BSFChat.png")
    draw_logo(512).save(out, format="PNG")
    print(f"  → {out}")


def main():
    print("Generating BSFChat brand icons…")
    # Master raster — committed to the repo, useful for README,
    # social cards, anywhere we need the pixel mark.
    master_path = os.path.join(HERE, "BSFChat-1024.png")
    draw_logo(1024).save(master_path, format="PNG")
    print(f"  → {master_path}")

    if sys.platform == "darwin":
        build_macos_icns()
    else:
        print("  (skipping .icns — needs macOS iconutil)")
    build_windows_ico()
    build_linux_png()
    print("Done.")


if __name__ == "__main__":
    main()
