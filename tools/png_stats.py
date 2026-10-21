"""PNG 平均亮度统计（诊断用，非构建产物）。

用法: python tools/png_stats.py <a.png> [b.png ...]

仅用标准库解码（zlib + struct），支持 8-bit RGBA/RGB 非隔行 PNG —— 足够阅读
本项目 lodepng/stb 写出的导出图。用于对比改动前后导出图的整体亮度
（例如离屏颜色附件 UNORM → SRGB 的 gamma 修正）。
"""

import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: " + path)
    pos = 8
    width = height = depth = None
    channels = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", body[:10])
            if depth not in (8, 16) or color_type not in (2, 6):
                raise ValueError("unsupported PNG: depth=%d color=%d" % (depth, color_type))
            channels = 3 if color_type == 2 else 4
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    raw = zlib.decompress(bytes(idat))
    bpp = channels * (depth // 8)          # 每像素字节数（滤波器用）
    stride = width * bpp
    out = bytearray(width * height * bpp)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif filt == 4:
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, depth, channels, out


def main(paths):
    for path in paths:
        try:
            w, h, depth, ch, px = read_png(path)
        except Exception as exc:                       # noqa: BLE001 (诊断脚本)
            print("%-40s ERROR %s" % (path, exc))
            continue
        step = depth // 8                              # 每通道字节数
        total = 0
        nonbg = 0
        n = w * h
        # 背景参考 = 左上角像素；容差随位深缩放（16-bit 的 24 相当于 8-bit 的 ~0.09）
        bg = (px[0] << 8 | px[1], px[step] << 8 | px[step + 1],
              px[2 * step] << 8 | px[2 * step + 1]) if depth == 16 else tuple(px[0:3])
        tol = 24 * (256 if depth == 16 else 1)
        for i in range(0, n * ch * step, ch * step):
            r = px[i] << 8 | px[i + 1] if depth == 16 else px[i]
            g = (px[i + step] << 8 | px[i + step + 1]) if depth == 16 else px[i + 1]
            b = (px[i + 2 * step] << 8 | px[i + 2 * step + 1]) if depth == 16 else px[i + 2]
            scale = 65535.0 if depth == 16 else 255.0
            total += (r + g + b) / scale
            if abs(r - bg[0]) > tol or abs(g - bg[1]) > tol or abs(b - bg[2]) > tol:
                nonbg += 1
        print("%-30s %dx%d %2d-bit mean=%.4f nonBg=%d (%.1f%%)"
              % (path, w, h, depth, total / (n * 3.0), nonbg, 100.0 * nonbg / n))


if __name__ == "__main__":
    main(sys.argv[1:])
