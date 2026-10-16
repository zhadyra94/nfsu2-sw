"""Compose the NFSU1 app icon: logo (RGBA, transparent) centred on a dark
radial gradient, 1024x1024 RGB (no alpha), plus the loose PNG sizes. The
logo's white letters get a silver two-band gradient; its black outline stays.

usage: python3 make_icon.py LOGO.png REPO_DIR [width_frac] [rgb_centre] [rgb_edge]
"""
import math
import sys

from PIL import Image, ImageFilter

logo_path, repo = sys.argv[1], sys.argv[2]
frac = float(sys.argv[3]) if len(sys.argv) > 3 else 0.80
centre = tuple(int(v) for v in (sys.argv[4] if len(sys.argv) > 4 else "16,34,30").split(","))
edge = tuple(int(v) for v in (sys.argv[5] if len(sys.argv) > 5 else "5,8,8").split(","))

N = 1024
bg = Image.new("RGB", (N, N))
px = bg.load()
for y in range(N):
    for x in range(N):
        d = math.hypot(x - N / 2, (y - N / 2) * 1.15) / (N * 0.62)
        t = min(1.0, d) ** 1.4
        px[x, y] = tuple(int(c * (1 - t) + e * t) for c, e in zip(centre, edge))

logo = Image.open(logo_path).convert("RGBA")
bbox = logo.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
if bbox:
    logo = logo.crop(bbox)
# Chrome letters: light top, darker band at the horizon, bright bottom.
def chrome(t):
    if t < 0.52:
        u, a, b = t / 0.52, (255, 255, 255), (196, 206, 214)
    else:
        u, a, b = (t - 0.52) / 0.48, (122, 138, 150), (232, 238, 242)
    return tuple(int(p + (q - p) * u) for p, q in zip(a, b))

lp = logo.load()
for y in range(logo.height):
    c = chrome(y / (logo.height - 1))
    for x in range(logo.width):
        r, g, b, al = lp[x, y]
        lum = (r + g + b) / 765
        if al and lum > 0.05:
            lp[x, y] = (int(c[0] * lum), int(c[1] * lum), int(c[2] * lum), al)

w = int(N * frac)
h = int(logo.height * w / logo.width)
if h > N * 0.55:
    h = int(N * 0.55)
    w = int(logo.width * h / logo.height)
logo = logo.resize((w, h), Image.LANCZOS)

# Soft glow from the logo's own alpha, like the NFSU2 icon.
glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ox, oy = (N - w) // 2, (N - h) // 2
a = logo.getchannel("A")
tint = Image.new("RGBA", logo.size, (120, 220, 200, 0))
tint.putalpha(a.point(lambda v: v * 0.55))
glow.paste(tint, (ox, oy), tint)
glow = glow.filter(ImageFilter.GaussianBlur(18))

out = bg.convert("RGBA")
out.alpha_composite(glow)
out.alpha_composite(logo, (ox, oy))
out = out.convert("RGB")

out.save(f"{repo}/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png", optimize=True)
for name, size in (("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180),
                   ("AppIcon76x76@2x~ipad.png", 152), ("AppIcon83.5x83.5@2x~ipad.png", 167)):
    out.resize((size, size), Image.LANCZOS).save(f"{repo}/ios/icon/{name}", optimize=True)
print("logo", logo.size, "at", (ox, oy))
