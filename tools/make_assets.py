"""Generate fallback PKG art for PS4 React.

Run on a workstation with Pillow:
    python tools/make_assets.py
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)

CYAN = (56, 214, 240)
DARK = (8, 12, 28)
MID = (24, 18, 56)
WHITE = (245, 248, 255)
MUTED = (150, 165, 190)


def font(size: int, bold: bool = True) -> ImageFont.FreeTypeFont:
    candidates = [
        "C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size=size)


def gradient(width: int, height: int) -> Image.Image:
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(1, height - 1)
        color = tuple(round(DARK[i] * (1 - t) + MID[i] * t) for i in range(3))
        draw.line((0, y, width, y), fill=color)
    return image


def grid(draw: ImageDraw.ImageDraw, width: int, height: int) -> None:
    for x in range(0, width, 120):
        draw.line((x, 0, x, height), fill=(34, 40, 62), width=1)
    for y in range(0, height, 120):
        draw.line((0, y, width, y), fill=(34, 40, 62), width=1)


def icon() -> None:
    size = 512
    image = gradient(size, size)
    draw = ImageDraw.Draw(image)
    grid(draw, size, size)
    draw.rounded_rectangle((48, 96, 464, 416), radius=36, outline=CYAN, width=8)
    draw.rounded_rectangle((72, 120, 440, 392), radius=24, fill=(14, 20, 38))
    draw.text((104, 148), "PS4", font=font(132), fill=WHITE)
    draw.text((104, 286), "REACT", font=font(74), fill=CYAN)
    draw.text((108, 372), "OpenOrbis", font=font(26, bold=False), fill=MUTED)
    image.save(ASSETS / "icon0.png", optimize=True)
    print(ASSETS / "icon0.png")


def splash(name: str, subtitle: str) -> None:
    width, height = 1920, 1080
    image = gradient(width, height)
    draw = ImageDraw.Draw(image)
    grid(draw, width, height)
    draw.rectangle((0, 0, width, 8), fill=CYAN)
    draw.rectangle((0, height - 8, width, height), fill=CYAN)
    draw.text((160, 300), "PS4 React", font=font(150), fill=WHITE)
    draw.text((166, 480), subtitle, font=font(54, bold=False), fill=MUTED)
    draw.text((166, 760), "OpenOrbis / PacBrew", font=font(40, bold=False), fill=CYAN)
    image.save(ASSETS / name, optimize=True)
    print(ASSETS / name)


if __name__ == "__main__":
    icon()
    splash("pic0.png", "A React UI runtime for PS4 homebrew")
    splash("pic1.png", "Starting...")
