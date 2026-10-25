#!/usr/bin/env python3
"""Анімована заставка запуску ПОТУЖНОГО РАДІО → assets/splash.yan (+ splash.gif для перегляду).

Кадри малюються тут, на комп'ютері, зі згладжуванням і світінням (Pillow),
а радіо лише відтворює готове: так і гарніше, і дешевше для процесора.

Формат YAN1 (усі числа little-endian):
  заголовок 16 байт: 'YAN1', w, h, кадрів, кадр/с, loopStart, wrap (індекс кадру переходу з кінця петлі
  на її початок), 0
  далі кадри: u32 довжина, потім прямокутники: u16 x, y, w, h (w == 0 — кінець кадру) і пікселі
  RGB565 у порядку дисплея (старший байт першим), стиснуті повторами: байт n < 128 — n+1 різних пікселів
  далі; n >= 128 — (n - 126) однакових, піксель один раз.
Кожен кадр — лише зміни відносно попереднього (квадрати 8×8, що змінились, зведені в смуги).

  python3 tools/make_splash.py            — ES3C28P: 320×156
  python3 tools/make_splash.py --large    — ES3C35P: 480×208 (той самий рисунок ×4/3)
"""
import math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 320, 156            # верхня частина екрана: нижче — рядок стану завантаження радіо
# --large: ES3C35P (480×320, інтерфейс ×4/3) — малюємо в тих самих логічних пікселях
# (ширина 360 = 480 / 4·3), а готовий кадр — 480×208, тобто все ×4/3.
LARGE = "--large" in sys.argv
LW, LH = (360, 156) if LARGE else (W, H)   # де малюємо (логічні пікселі)
if LARGE:
    W, H = 480, 208                        # що лягає в файл (пікселі панелі)
FPS = 25
SS = 4                     # надвибірка для згладжування
INTRO, LOOP = 50, 50
ACC = (230, 210, 90)       # #e6d25a — акцентний жовтий радіо
DIM = (150, 150, 150)
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "source", "yoRadio", "assets")
FONT = os.path.expanduser("~/Library/Fonts/Montserrat-Bold.otf")   # OFL, з українською кирилицею

CX, CY = LW // 2, 66       # центр значка (логічні пікселі)
TEXT = "ПОТУЖНЕ РАДІО"


def ease(t):
    t = max(0.0, min(1.0, t))
    return 1 - (1 - t) ** 3


def arc(d, cx, cy, r, a0, a1, width, color):
    box = [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS]
    d.arc(box, a0, a1, fill=color, width=int(width * SS))


def frame(i):
    """Кадр i як RGB-масив W×H."""
    t = i / FPS
    big = Image.new("RGB", (LW * SS, LH * SS), (0, 0, 0))
    glow = Image.new("RGB", (LW * SS, LH * SS), (0, 0, 0))
    dg = ImageDraw.Draw(glow)
    d = ImageDraw.Draw(big)

    # петля: кола, що розходяться від точки (після вступу)
    if i >= INTRO:
        ph = ((i - INTRO) % LOOP) / LOOP
        for k in range(2):
            p = (ph + k * 0.5) % 1.0
            r = 44 + p * 90
            a = int(70 * (1 - p) ** 1.6)
            if a > 2:
                c = tuple(int(ch * a / 255) for ch in ACC)
                box = [(CX - r) * SS, (CY - r) * SS, (CX + r) * SS, (CY + r) * SS]
                d.ellipse(box, outline=c, width=int(1.2 * SS))

    # точка
    pr = 9 * ease(t / 0.45)
    if pr > 0.3:
        dg.ellipse([(CX - pr * 2.2) * SS, (CY - pr * 2.2) * SS, (CX + pr * 2.2) * SS, (CY + pr * 2.2) * SS], fill=ACC)
        d.ellipse([(CX - pr) * SS, (CY - pr) * SS, (CX + pr) * SS, (CY + pr) * SS], fill=ACC)
    # щогла
    ml = 27 * ease((t - 0.25) / 0.6)
    if ml > 0.5:
        d.line([CX * SS, (CY + 11) * SS, CX * SS, (CY + 11 + ml) * SS], fill=ACC, width=int(3.2 * SS))
    # хвилі: внутрішня пара, потім зовнішня — «розгортаються» від горизонталі
    for (r, start, wdt) in ((22, 0.40, 3.2), (36, 0.62, 3.0)):
        p = ease((t - start) / 0.7)
        if p > 0.01:
            span = 46 * p
            for side in (0, 180):
                arc(d, CX, CY, r, side - span, side + span, wdt, ACC)
                arc(dg, CX, CY, r, side - span, side + span, wdt * 2, ACC)
    # напис
    ta = ease((t - 0.95) / 0.8)
    if ta > 0:
        font = ImageFont.truetype(FONT, 21 * SS)
        spacing = (3.0 + 5.0 * (1 - ta)) * SS
        widths = [font.getlength(ch) for ch in TEXT]
        total = sum(widths) + spacing * (len(TEXT) - 1)
        x = (LW * SS - total) / 2
        y = (136 - 6 * (1 - ta)) * SS
        col = tuple(int(c * ta) for c in ACC)
        for ch, wch in zip(TEXT, widths):
            d.text((x, y), ch, font=font, fill=col, anchor="ls")
            x += wch + spacing

    glow = glow.filter(ImageFilter.GaussianBlur(7 * SS))
    img = np.asarray(big).astype(np.float32) + np.asarray(glow).astype(np.float32) * 0.35
    img = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize((W, H), Image.LANCZOS)
    return np.asarray(img)


def rgb565(a):
    r = (a[..., 0].astype(np.uint16) >> 3) << 11
    g = (a[..., 1].astype(np.uint16) >> 2) << 5
    b = a[..., 2].astype(np.uint16) >> 3
    return r | g | b


def rle(px):
    """px — масив uint16 → байти (старший першим) зі стисненням повторів."""
    out = bytearray()
    n = len(px)
    i = 0
    lit = []

    def flush():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            out.append(len(chunk) - 1)
            for v in chunk:
                out.extend(struct.pack(">H", int(v)))

    while i < n:
        j = i
        while j + 1 < n and px[j + 1] == px[i] and j - i < 128:
            j += 1
        run = j - i + 1
        if run >= 3:
            flush()
            out.append(126 + min(run, 129))
            out += struct.pack(">H", int(px[i]))
            i += min(run, 129)
        else:
            lit.append(px[i])
            i += 1
    flush()
    return bytes(out)


def delta(prev, cur):
    """Прямокутники змін: квадрати 8×8 → смуги по рядку квадратів."""
    T = 8
    out = bytearray()
    ch = (prev != cur) if prev is not None else np.ones(cur.shape, bool)
    rows = (H + T - 1) // T
    cols = (W + T - 1) // T
    for ty in range(rows):
        y0, y1 = ty * T, min(H, (ty + 1) * T)
        flags = [bool(ch[y0:y1, tx * T:min(W, (tx + 1) * T)].any()) for tx in range(cols)]
        tx = 0
        while tx < cols:
            if not flags[tx]:
                tx += 1
                continue
            s = tx
            while tx < cols and flags[tx]:
                tx += 1
            x0, x1 = s * T, min(W, tx * T)
            out += struct.pack("<HHHH", x0, y0, x1 - x0, y1 - y0)
            out += rle(cur[y0:y1, x0:x1].reshape(-1))
    out += struct.pack("<HHHH", 0, 0, 0, 0)
    return bytes(out)


def main():
    frames = [frame(i) for i in range(INTRO + LOOP)]
    px = [rgb565(f) for f in frames]
    blobs = []
    for i, p in enumerate(px):
        blobs.append(delta(px[i - 1] if i else None, p))
    wrap = delta(px[-1], px[INTRO])          # кінець петлі → її початок
    blobs.append(wrap)
    data = bytearray(struct.pack("<4sHHHHHH", b"YAN1", W, H, len(blobs), FPS, INTRO, len(blobs) - 1))
    for b in blobs:
        data += struct.pack("<I", len(b)) + b
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "splash.yan")
    open(path, "wb").write(data)
    print(f"{path}: {len(data)} байт, кадрів {len(blobs)}, найбільший кадр {max(len(b) for b in blobs)} байт")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    gif = os.path.join(args[0] if args else HERE, "splash.gif")
    imgs = [Image.fromarray(f).resize((W * 2, H * 2), Image.NEAREST) for f in frames + frames[INTRO:]]
    imgs[0].save(gif, save_all=True, append_images=imgs[1:], duration=1000 // FPS, loop=0)
    print("перегляд:", gif)


if __name__ == "__main__":
    main()
