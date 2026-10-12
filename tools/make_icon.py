"""Generates src/app.ico (run once; the .ico is checked in)."""
import sys
from PIL import Image, ImageDraw

SIZE = 256
SS = 4  # supersampling factor


def draw(size):
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = big * 0.04
    d.rounded_rectangle([pad, pad, big - pad, big - pad], radius=big * 0.22, fill=(30, 41, 79, 255))
    # waveform bars
    heights = [0.22, 0.42, 0.66, 0.42, 0.30, 0.55, 0.26]
    n = len(heights)
    left, right = big * 0.17, big * 0.83
    step = (right - left) / n
    bar = step * 0.56
    cy = big * 0.47
    for i, h in enumerate(heights):
        x = left + step * i + (step - bar) / 2
        half = big * h / 2
        d.rounded_rectangle([x, cy - half, x + bar, cy + half], radius=bar / 2, fill=(120, 200, 255, 255))
    # record dot
    r = big * 0.13
    cx, cy2 = big * 0.74, big * 0.76
    d.ellipse([cx - r - big * 0.025, cy2 - r - big * 0.025, cx + r + big * 0.025, cy2 + r + big * 0.025],
              fill=(30, 41, 79, 255))
    d.ellipse([cx - r, cy2 - r, cx + r, cy2 + r], fill=(239, 68, 68, 255))
    return img.resize((size, size), Image.LANCZOS)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "src/app.ico"
    base = draw(SIZE)
    base.save(out, format="ICO", sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)])
    print("wrote", out)
