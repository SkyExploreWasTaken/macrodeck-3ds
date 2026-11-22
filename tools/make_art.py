"""Generates meta/icon.png (48x48), meta/banner.png (256x128) and meta/audio.wav for the CIA/3DSX."""
import os, struct, wave
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
META = os.path.join(ROOT, "meta")
FONT = "/usr/share/fonts/opentype/inter/Inter-Bold.otf"
KEYS = [(0x4F, 0x7C, 0xFF), (0x3C, 0xD0, 0x7A), (0xF5, 0xB7, 0x2E), (0xF0, 0x52, 0x52), (0xA8, 0x6B, 0xFF), (0x2E, 0xC4, 0xE0)]


def keys(draw, x, y, cell, gap, radius):
    for i, c in enumerate(KEYS):
        cx, cy = x + (i % 3) * (cell + gap), y + (i // 3) * (cell + gap)
        draw.rounded_rectangle([cx, cy, cx + cell, cy + cell], radius=radius, fill=c + (255,))


def icon():
    s = 4  # supersample
    im = Image.new("RGBA", (48 * s, 48 * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, 48 * s - 1, 48 * s - 1], radius=10 * s, fill=(0x16, 0x1A, 0x22, 255))
    keys(d, 6 * s, 12 * s, 10 * s, 3 * s, 3 * s)
    im.resize((48, 48), Image.LANCZOS).save(os.path.join(META, "icon.png"))


def banner():
    s = 2
    w, h = 256 * s, 128 * s
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([8 * s, 16 * s, w - 8 * s, h - 16 * s], radius=14 * s, fill=(0x16, 0x1A, 0x22, 255))
    keys(d, 22 * s, 36 * s, 22 * s, 5 * s, 5 * s)
    big = ImageFont.truetype(FONT, 21 * s)
    small = ImageFont.truetype(FONT.replace("Bold", "Medium"), 12 * s)
    d.text((104 * s, 40 * s), "Macro Deck", font=big, fill=(0xF2, 0xF4, 0xF8, 255))
    d.text((105 * s, 70 * s), "for Nintendo 3DS", font=small, fill=(0x9A, 0xA3, 0xB5, 255))
    im.resize((256, 128), Image.LANCZOS).save(os.path.join(META, "banner.png"))


def audio():
    # A short soft two-note chime (CIA banners need a sound; keep it gentle).
    rate, out = 22050, []
    import math
    for freq, length in ((880, 0.12), (1320, 0.22)):
        n = int(rate * length)
        for i in range(n):
            env = min(1.0, i / 200) * (1 - i / n) ** 2
            out.append(int(9000 * env * math.sin(2 * math.pi * freq * i / rate)))
    out += [0] * int(rate * 0.3)
    with wave.open(os.path.join(META, "audio.wav"), "wb") as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(rate)
        wv.writeframes(b"".join(struct.pack("<h", v) for v in out))


if __name__ == "__main__":
    os.makedirs(META, exist_ok=True)
    icon()
    banner()
    audio()
