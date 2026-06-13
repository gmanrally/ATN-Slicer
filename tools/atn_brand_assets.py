"""Generate ATN-branded app assets (icons + splash) from the ATN brand.

Run once after changing the brand. Outputs into resources/images/:
  - OrcaSlicer.ico, OrcaSlicer-mac_256px.ico, OrcaSlicerTitle.ico  (app/window/taskbar icon)
  - splash_atn.png                                                 (480x480 startup splash)

Source art: print-doctor's ATN PWA icon + the brand teal. No external deps beyond Pillow.
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

IMAGES = Path(__file__).resolve().parent.parent / "resources" / "images"
ATN_ICON = Path(r"c:/Users/Graham Work/print-doctor/static/pwa-icons/icon-512.png")

TEAL_TOP = (11, 110, 110)     # #0b6e6e
TEAL_BOT = (10, 140, 140)     # #0a8c8c
WHITE = (255, 255, 255)
TAGLINE = (191, 227, 224)     # soft teal


def _font(names, size):
    for n in names:
        try:
            return ImageFont.truetype(n, size)
        except OSError:
            continue
    return ImageFont.load_default()


def make_icons():
    src = Image.open(ATN_ICON).convert("RGBA")
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    for name in ("OrcaSlicer.ico", "OrcaSlicer-mac_256px.ico", "OrcaSlicerTitle.ico"):
        src.save(IMAGES / name, sizes=sizes)
        print("wrote", name)


def _draw_nozzle(d, cx, cy, scale, colour):
    """White nozzle mark (favicon geometry) centred at (cx, cy)."""
    def P(x, y):  # favicon 32-space -> splash space, nozzle centred on (16, 16.75)
        return (cx + (x - 16) * scale, cy + (y - 16.75) * scale)
    hexagon = [P(10, 5.5), P(22, 5.5), P(22, 11.5), P(18.5, 18), P(13.5, 18), P(10, 11.5)]
    d.polygon(hexagon, fill=colour)
    d.line([P(16, 18), P(16, 24)], fill=colour, width=int(2.4 * scale), joint="curve")
    r = 1.1 * scale
    c = P(16, 27)
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=colour)


def make_splash():
    W = H = 480
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):  # vertical teal gradient
        t = y / H
        px_row = tuple(int(TEAL_TOP[i] + (TEAL_BOT[i] - TEAL_TOP[i]) * t) for i in range(3))
        for x in range(W):
            px[x, y] = px_row
    d = ImageDraw.Draw(img)

    _draw_nozzle(d, cx=240, cy=150, scale=8.0, colour=WHITE)

    wordmark = _font(["segoeuib.ttf", "arialbd.ttf", "Arialbd.ttf"], 58)
    tag = _font(["segoeui.ttf", "arial.ttf", "Arial.ttf"], 20)
    title = "ATN Slicer"
    tw = d.textlength(title, font=wordmark)
    d.text(((W - tw) / 2, 250), title, font=wordmark, fill=WHITE)
    sub = "Forked from OrcaSlicer · pre-flight built in"
    sw = d.textlength(sub, font=tag)
    d.text(((W - sw) / 2, 312), sub, font=tag, fill=TAGLINE)

    img.save(IMAGES / "splash_atn.png")
    print("wrote splash_atn.png")


if __name__ == "__main__":
    make_icons()
    make_splash()
