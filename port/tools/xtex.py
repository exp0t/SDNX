#!/usr/bin/env python3
"""Xbox 360 (Xenos) 2D texture helpers: tiling, 8-in-16 endian swap, DXT1/DXT5 decode and DXT5 encode.

Tiling is XGAddress2DTiledX/Y (the formula Xenia uses), applied on 4x4 blocks for DXT formats. Tiled
surfaces are stored with both dimensions aligned to 32 blocks.
"""
import numpy as np


def _log_bpp(texel_pitch):
    return (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2))


def tiled_xy(offset, width, texel_pitch):
    aligned_w = (width + 31) & ~31
    lb = _log_bpp(texel_pitch)
    ob = offset << lb
    ot = ((ob & ~4095) >> 3) + ((ob & 1792) >> 2) + (ob & 63)
    om = ot >> (7 + lb)
    macro_x = (om % (aligned_w >> 5)) << 2
    tile = ((((ot >> (5 + lb)) & 2) + (ob >> 6)) & 3)
    macro = (macro_x + tile) << 3
    micro = ((((ot >> 1) & ~15) + (ot & 15)) & ((texel_pitch << 3) - 1)) >> lb
    x = macro + micro

    macro_y = (om // (aligned_w >> 5)) << 2
    tile = ((ot >> (6 + lb)) & 1) + ((ob & 2048) >> 10)
    macro = (macro_y + tile) << 3
    micro = ((((ot & (((texel_pitch << 6) - 1) & ~31)) + ((ot & 15) << 1)) >> (3 + lb)) & ~1)
    y = macro + micro + ((ot & 16) >> 4)
    return x, y


def swap16(b: bytes) -> bytes:
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


def untile_blocks(data: bytes, wb: int, hb: int, bsize: int) -> bytes:
    """Tiled block stream (aligned to 32x32 blocks) -> linear wb x hb blocks."""
    aw = (wb + 31) & ~31
    ah = (hb + 31) & ~31
    out = bytearray(wb * hb * bsize)
    for i in range(aw * ah):
        x, y = tiled_xy(i, aw, bsize)
        if x < wb and y < hb:
            out[(y * wb + x) * bsize:(y * wb + x + 1) * bsize] = data[i * bsize:(i + 1) * bsize]
    return bytes(out)


def tile_blocks(lin: bytes, wb: int, hb: int, bsize: int, total: int, base: bytes = None) -> bytes:
    """Linear blocks -> tiled stream of `total` bytes; `base` (an existing tiled stream) keeps its padding."""
    aw = (wb + 31) & ~31
    ah = (hb + 31) & ~31
    out = bytearray(base[:total]) if base is not None else bytearray(total)
    for i in range(aw * ah):
        x, y = tiled_xy(i, aw, bsize)
        if x < wb and y < hb and (i + 1) * bsize <= total:
            out[i * bsize:(i + 1) * bsize] = lin[(y * wb + x) * bsize:(y * wb + x + 1) * bsize]
    return bytes(out)


# ---------------------------------------------------------------------------- DXT (little-endian blocks)

def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], dtype=np.int32)


def _color_block(blk, out, dxt1):
    c0 = blk[0] | (blk[1] << 8)
    c1 = blk[2] | (blk[3] << 8)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    pal = [p0, p1]
    alpha = [255, 255, 255, 255]
    if c0 > c1 or not dxt1:
        pal += [(2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
    else:
        pal += [(p0 + p1) // 2, np.zeros(3, np.int32)]
        alpha[3] = 0
    bits = blk[4] | (blk[5] << 8) | (blk[6] << 16) | (blk[7] << 24)
    for i in range(16):
        k = (bits >> (2 * i)) & 3
        out[i // 4, i % 4, :3] = pal[k]
        if dxt1:
            out[i // 4, i % 4, 3] = alpha[k]


def _alpha_block(blk, out):
    a0, a1 = blk[0], blk[1]
    pal = [a0, a1]
    if a0 > a1:
        pal += [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        pal += [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    bits = int.from_bytes(blk[2:8], "little")
    for i in range(16):
        out[i // 4, i % 4, 3] = pal[(bits >> (3 * i)) & 7]


def decode_dxt(lin: bytes, w: int, h: int, fmt: str) -> np.ndarray:
    bsize = 8 if fmt == "DXT1" else 16
    wb, hb = (w + 3) // 4, (h + 3) // 4
    img = np.zeros((hb * 4, wb * 4, 4), np.uint8)
    tmp = np.zeros((4, 4, 4), np.int32)
    for by in range(hb):
        for bx in range(wb):
            o = (by * wb + bx) * bsize
            blk = lin[o:o + bsize]
            tmp[...] = 255
            if fmt == "DXT1":
                _color_block(blk, tmp, True)
            else:
                _alpha_block(blk[:8], tmp)
                _color_block(blk[8:], tmp, False)
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = tmp
    return img[:h, :w]


def _to565(c):
    r, g, b = [int(v) for v in c]
    return ((r * 31 + 127) // 255 << 11) | ((g * 63 + 127) // 255 << 5) | ((b * 31 + 127) // 255)


def encode_dxt5(img: np.ndarray) -> bytes:
    """Straightforward DXT5 encoder (bounding-box endpoints, nearest palette entry). Good enough for flat UI icons."""
    h, w = img.shape[:2]
    wb, hb = (w + 3) // 4, (h + 3) // 4
    out = bytearray()
    for by in range(hb):
        for bx in range(wb):
            blk = img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4].reshape(-1, 4).astype(np.int32)
            # alpha
            a = blk[:, 3]
            a0, a1 = int(a.max()), int(a.min())
            if a0 == a1:
                out += bytes([a0, a1]) + b"\0" * 6
            else:
                pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
                bits = 0
                for i, v in enumerate(a):
                    k = min(range(8), key=lambda j: abs(pal[j] - v))
                    bits |= k << (3 * i)
                out += bytes([a0, a1]) + bits.to_bytes(6, "little")
            # colour: weight by alpha so transparent texels do not pull the endpoints
            rgb = blk[:, :3]
            vis = rgb[a > 16] if (a > 16).any() else rgb
            lo, hi = vis.min(axis=0), vis.max(axis=0)
            c0, c1 = _to565(hi), _to565(lo)
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:
                out += c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + b"\0" * 4
                continue
            p0, p1 = _rgb565(c0), _rgb565(c1)
            pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
            bits = 0
            for i, v in enumerate(rgb):
                k = min(range(4), key=lambda j: int(((pal[j] - v) ** 2).sum()))
                bits |= k << (2 * i)
            out += c0.to_bytes(2, "little") + c1.to_bytes(2, "little") + bits.to_bytes(4, "little")
    return bytes(out)
