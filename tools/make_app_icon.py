"""Generate the MolecuLens app icon (deep blue-violet, lens + molecule).

The icon is drawn at 4x then downsampled so the 16 px taskbar frame stays clean.
Run from the repository root:  python tools/make_app_icon.py
"""
from __future__ import annotations

import math
import os
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "design" / "logo"
APP_ICO = ROOT / "src" / "app" / "app.ico"

S = 4096                      # supersampled canvas
FINAL = 1024
C = S / 2.0


def lerp(a, b, t):
    return a + (b - a) * t


def rounded_mask(size: int, radius_frac: float = 0.225) -> Image.Image:
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    r = int(size * radius_frac)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=r, fill=255)
    return m


def gradient_background(size: int) -> Image.Image:
    # Diagonal deep-blue -> indigo -> violet field.
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    t = np.clip((x / size) * 0.58 + (y / size) * 0.42, 0.0, 1.0)
    stops = [
        (0.00, np.array([7, 12, 38], np.float32)),
        (0.45, np.array([20, 17, 66], np.float32)),
        (0.78, np.array([48, 25, 105], np.float32)),
        (1.00, np.array([31, 18, 78], np.float32)),
    ]
    rgb = np.zeros((size, size, 3), np.float32)
    for i in range(len(stops) - 1):
        a, ca = stops[i]
        b, cb = stops[i + 1]
        m = (t >= a) & (t <= b)
        local = np.clip((t - a) / (b - a), 0, 1)
        for k in range(3):
            rgb[..., k][m] = lerp(ca[k], cb[k], local[m])

    # Soft violet bloom low-centre and a cooler blue bloom top-left.
    for cx, cy, rad, col, strength in [
        (0.50, 0.60, 0.62, np.array([124, 92, 255], np.float32), 0.34),
        (0.26, 0.22, 0.50, np.array([56, 130, 255], np.float32), 0.20),
    ]:
        d = np.sqrt(((x / size - cx) ** 2 + (y / size - cy) ** 2)) / rad
        g = np.clip(1.0 - d, 0, 1) ** 2.0 * strength
        rgb += g[..., None] * col[None, None, :]

    # Vignette keeps the tile edge dark (no light rim).
    edge = np.minimum(np.minimum(x, size - 1 - x), np.minimum(y, size - 1 - y)) / size
    vig = np.clip(edge / 0.10, 0, 1)[..., None]
    rgb *= 0.72 + 0.28 * vig
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    return Image.fromarray(rgb, "RGB").convert("RGBA")


def add_glow(base: Image.Image, layer: Image.Image, blur: float) -> None:
    glow = layer.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(glow)
    base.alpha_composite(layer)


def star(draw: ImageDraw.ImageDraw, cx: float, cy: float, r: float, color):
    # Four-point sparkle.
    k = r * 0.20
    pts = [
        (cx, cy - r), (cx + k, cy - k), (cx + r, cy), (cx + k, cy + k),
        (cx, cy + r), (cx - k, cy + k), (cx - r, cy), (cx - k, cy - k),
    ]
    draw.polygon(pts, fill=color)


def draw_icon(detail: int = 2) -> Image.Image:
    """detail 2 = 256–1024 px, 1 = 48–128 px, 0 = 16–32 px.

    Small frames drop the hexagon/orbital/sparkles and enlarge the molecule so the
    mark still reads in the taskbar instead of collapsing into a bright blob.
    """
    bg = gradient_background(S)
    mask = rounded_mask(S, 0.225)

    # Inner subtle rim: dark violet, inset, never a white outline.
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rim)
    inset = int(S * 0.012)
    rd.rounded_rectangle(
        (inset, inset, S - 1 - inset, S - 1 - inset),
        radius=int(S * 0.215),
        outline=(126, 96, 235, 105),
        width=max(2, int(S * 0.0035)),
    )
    bg.alpha_composite(rim)

    # Tinted lens glass behind the mark (large/medium frames only).
    if detail >= 1:
        glass = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glass)
        gr = S * 0.360
        gd.ellipse((C - gr, C * 0.94 - gr, C + gr, C * 0.94 + gr),
                   fill=(82, 70, 176, 58))
        gr2 = S * 0.330
        gd.ellipse((C - gr2, C * 0.94 - gr2, C + gr2, C * 0.94 + gr2),
                   fill=(108, 92, 220, 34))
        # Specular crescent at upper-left.
        gd.arc((C - gr * 0.96, C * 0.94 - gr * 0.96,
                C + gr * 0.96, C * 0.94 + gr * 0.96),
               start=196, end=252, fill=(220, 230, 255, 52),
               width=int(S * 0.010))
        bg.alpha_composite(glass)

    # --- Benzene / aromatic hexagon (structure hint) -------------------------
    hex_layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hex_layer)
    hx, hy = C, C * 0.94
    hr = S * 0.300
    rot = math.radians(30)
    pts = [
        (hx + hr * math.cos(rot + i * math.pi / 3),
         hy + hr * math.sin(rot + i * math.pi / 3))
        for i in range(6)
    ]
    hd.line(pts + [pts[0]], fill=(167, 139, 250, 150), width=int(S * 0.010),
            joint="curve")
    # Inner faint hexagon for depth.
    ir = hr * 0.82
    ipts = [
        (hx + ir * math.cos(rot + i * math.pi / 3),
         hy + ir * math.sin(rot + i * math.pi / 3))
        for i in range(6)
    ]
    hd.line(ipts + [ipts[0]], fill=(125, 211, 252, 60), width=int(S * 0.0035),
            joint="curve")
    # Vertex atoms: small violet/cyan pearls.
    for i, (px, py) in enumerate(pts):
        col = (196, 181, 253, 220) if i % 2 == 0 else (103, 232, 249, 200)
        rr = S * 0.0105
        hd.ellipse((px - rr, py - rr, px + rr, py + rr), fill=col)
    if detail >= 2:
        add_glow(bg, hex_layer, S * 0.010)

    # --- Orbital ellipse (scientific, adds complexity) ----------------------
    orb = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    od = ImageDraw.Draw(orb)
    for ang, alpha, width in [(34, 120, 0.0060), (-34, 85, 0.0045)]:
        a = math.radians(ang)
        rx, ry = S * 0.365, S * 0.150
        # Rotated ellipse as polyline.
        epts = []
        for i in range(160):
            t = 2 * math.pi * i / 160
            ex, ey = rx * math.cos(t), ry * math.sin(t)
            epts.append((C + ex * math.cos(a) - ey * math.sin(a),
                         C * 0.94 + ex * math.sin(a) + ey * math.cos(a)))
        od.line(epts, fill=(103, 232, 249, alpha), width=max(2, int(S * width)))
    if detail >= 2:
        add_glow(bg, orb, S * 0.009)

    # --- Lens arc: violet/icy aperture, deliberately NOT a white rim -------
    lens = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lens)
    if detail >= 1:
        lr = S * 0.372
        bbox = (C - lr, C * 0.94 - lr, C + lr, C * 0.94 + lr)
        # 300 degree arc, gap at lower-right so it reads as a lens/aperture.
        ld.ellipse(bbox, outline=(110, 200, 255, 70),
                   width=int(S * 0.024))
        ld.ellipse(bbox, outline=(146, 134, 255, 225),
                   width=int(S * 0.011))
        lr2 = lr * 0.905
        bbox2 = (C - lr2, C * 0.94 - lr2, C + lr2, C * 0.94 + lr2)
        ld.arc(bbox2, start=150, end=30, fill=(125, 211, 252, 80),
               width=max(2, int(S * 0.0040)))
        # Magnifier handle at 45 degrees, attached to the rim.
        a = math.radians(45)
        hx0, hy0 = C + lr * math.cos(a), C * 0.94 + lr * math.sin(a)
        hx1, hy1 = C + lr * 1.52 * math.cos(a), C * 0.94 + lr * 1.52 * math.sin(a)
        ld.line((hx0, hy0, hx1, hy1), fill=(146, 134, 255, 225),
                width=int(S * 0.030))
    else:
        # Tiny 16 px: no glass/atom/halo, just a bold magnifier silhouette so it
        # stays legible in the taskbar instead of becoming a purple blob.
        lr = S * 0.285
        bbox = (C - lr, C * 0.94 - lr, C + lr, C * 0.94 + lr)
        ld.ellipse(bbox, outline=(96, 196, 255, 110),
                   width=int(S * 0.100))
        ld.ellipse(bbox, outline=(196, 208, 255, 250),
                   width=int(S * 0.062))
        a = math.radians(45)
        hx0, hy0 = C + lr * 0.98 * math.cos(a), C * 0.94 + lr * 0.98 * math.sin(a)
        hx1, hy1 = C + lr * 1.82 * math.cos(a), C * 0.94 + lr * 1.82 * math.sin(a)
        ld.line((hx0, hy0, hx1, hy1), fill=(196, 208, 255, 250),
                width=int(S * 0.082))
    add_glow(bg, lens, S * 0.012)

    # --- Molecule: one centre atom + two bonded atoms ----------------------
    mol = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    md = ImageDraw.Draw(mol)
    if detail < 0:
        cx, cy = C, C * 0.94
    elif detail == 0:
        # 24/32 px: one bright atom inside the magnifier; a 3-atom cluster
        # turns into a "face" at this size.
        cx, cy = C, C * 0.94
        rr = S * 0.045
        md.ellipse((cx - rr, cy - rr, cx + rr, cy + rr),
                   fill=(230, 240, 255, 255))
        # A faint orbital ring around the tiny sample atom.
        orb_r = S * 0.085
        md.arc((cx - orb_r, cy - orb_r, cx + orb_r, cy + orb_r),
               start=205, end=335, fill=(125, 211, 252, 220),
               width=max(2, int(S * 0.012)))
        for ex, ey, er in [(cx - S * 0.072, cy + S * 0.038, 0.014),
                           (cx + S * 0.070, cy - S * 0.030, 0.012)]:
            md.ellipse((ex - S * er, ey - S * er, ex + S * er, ey + S * er),
                       fill=(125, 211, 252, 235))
    else:
        if detail == 1:
            cx, cy = C, C * 0.87
            bond = int(S * 0.036)
            off, oy = 0.168, 0.135
            r_center, r_side = 0.108, 0.080
        else:
            cx, cy = C, C * 0.86
            bond = int(S * 0.030)
            off, oy = 0.150, 0.120
            r_center, r_side = 0.092, 0.066
        left = (cx - S * off, cy + S * oy)
        right = (cx + S * off, cy + S * oy)
        # Bonds with a soft icy outline then a bright core.
        for ex, ey in (left, right):
            md.line((cx, cy, ex, ey), fill=(186, 205, 255, 235), width=bond)
            md.line((cx, cy, ex, ey), fill=(255, 255, 255, 235), width=max(2, int(bond * 0.42)))
        # Atoms: white cores with blue-violet halo.
        for (ax, ay), r in [((cx, cy), r_center), (left, r_side), (right, r_side)]:
            rr = S * r
            md.ellipse((ax - rr, ay - rr, ax + rr, ay + rr), fill=(245, 248, 255, 255))
            md.arc((ax - rr * 0.9, ay - rr * 0.9, ax + rr * 0.9, ay + rr * 0.9),
                   start=20, end=160, fill=(167, 180, 255, 130),
                   width=max(2, int(S * 0.004)))
    add_glow(bg, mol, S * 0.014)

    # Central violet bloom behind the molecule (not for the tiny silhouette).
    if detail >= 0:
        bloom = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        bd = ImageDraw.Draw(bloom)
        br = S * 0.20
        bd.ellipse((cx - br, cy - br, cx + br, cy + br), fill=(124, 92, 255, 70))
        add_glow(bg, bloom, S * 0.045)

    # --- Sparkles ----------------------------------------------------------
    sp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sp)
    star(sd, S * 0.790, S * 0.245, S * 0.028, (255, 255, 255, 235))
    star(sd, S * 0.205, S * 0.760, S * 0.020, (125, 211, 252, 220))
    star(sd, S * 0.795, S * 0.760, S * 0.014, (196, 181, 253, 210))
    if detail >= 2:
        add_glow(bg, sp, S * 0.006)

    # Apply rounded mask, then downsample.
    bg.putalpha(Image.composite(bg.getchannel("A"), Image.new("L", (S, S), 0), mask))
    icon = bg.resize((FINAL, FINAL), Image.LANCZOS)
    return icon


def write_ico(frames: list[tuple[int, Image.Image]], path: Path) -> None:
    """Write a PNG-compressed ICO with per-size artwork (Vista+ format)."""
    import struct

    blobs = []
    for _, im in frames:
        import io
        buf = io.BytesIO()
        im.save(buf, format="PNG")
        blobs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries = b""
    for (n, _), blob in zip(frames, blobs):
        w = 0 if n >= 256 else n
        h = 0 if n >= 256 else n
        entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32,
                               len(blob), offset)
        offset += len(blob)
    path.write_bytes(header + entries + b"".join(blobs))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    big = draw_icon(2)
    mid = draw_icon(1)
    small = draw_icon(0)
    tiny = draw_icon(-1)
    big.save(OUT / "icon-1024.png")
    big.resize((512, 512), Image.LANCZOS).save(OUT / "logo-512.png")

    sizes = {
        16: tiny, 24: small, 32: small,
        48: mid, 64: mid, 128: big,
        256: big, 512: big,
    }
    for n, base in sizes.items():
        base.resize((n, n), Image.LANCZOS).save(OUT / f"size-{n}.png")

    ico_frames = [(n, sizes[n].resize((n, n), Image.LANCZOS))
                  for n in (16, 24, 32, 48, 64, 128, 256)]
    write_ico(ico_frames, APP_ICO)
    (OUT / "app.ico").write_bytes(APP_ICO.read_bytes())
    print("wrote", APP_ICO)
    print("wrote", OUT / "icon-1024.png")


if __name__ == "__main__":
    main()
