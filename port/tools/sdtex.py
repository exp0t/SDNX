#!/usr/bin/env python3
"""Illusion::Texture headers (chunk 0xCDBFA090) in a Sleeping Dogs X360 .perm stream.

Layout of the 0x120-byte payload (offsets from the chunk start, big-endian), found on the retail data:
  +0x2C name[36]       +0x50 flags        +0x54 format (s8: 0 ARGB8, 1 DXT1, 2 DXT3, 3 DXT5, ...)
  +0x5C u16 width, u16 height             +0x60 s8 mips
  +0x80 u32 image data position (in the paired .temp stream)   +0x84 u32 image data byte size
  +0xA0.. Xenos texture fetch constant (GPUTEXTURE_FETCH_CONSTANT, 6 dwords)

Usage: sdtex.py perm.bin
"""
import struct
import sys

CHUNK_TEXTURE = 0xCDBFA090
FORMATS = {0: "A8R8G8B8", 1: "DXT1", 2: "DXT3", 3: "DXT5", 4: "R5G6B5", 5: "A1R5G5B5", 6: "X8", 7: "X16",
           8: "CXT1", 9: "DXN"}


def iter_textures(d):
    p = 0
    while p + 16 <= len(d):
        uid, size, dsize, doff = struct.unpack_from(">IIII", d, p)
        if uid == 0 and size == 0:
            break
        if uid == CHUNK_TEXTURE:
            name = d[p + 0x2C:p + 0x2C + 36].split(b"\0", 1)[0].decode("ascii", "replace")
            flags = struct.unpack_from(">I", d, p + 0x50)[0]
            fmt = d[p + 0x54]
            w, h = struct.unpack_from(">HH", d, p + 0x5C)
            mips = d[p + 0x60]
            pos, nbytes = struct.unpack_from(">II", d, p + 0x80)
            fetch = struct.unpack_from(">6I", d, p + 0xA0)
            yield dict(off=p, name=name, flags=flags, fmt=fmt, w=w, h=h, mips=mips, pos=pos, size=nbytes,
                       fetch=fetch)
        p += 16 + size


def main():
    d = open(sys.argv[1], "rb").read()
    end = 0
    for t in iter_textures(d):
        end = max(end, t["pos"] + t["size"])
        print("+%05X %-24s %-8s %4dx%-4d mips=%d flags=%X data=%08X+%06X fetch=%s" % (
            t["off"], t["name"], FORMATS.get(t["fmt"], t["fmt"]), t["w"], t["h"], t["mips"], t["flags"], t["pos"],
            t["size"], " ".join("%08X" % x for x in t["fetch"])))
    print("temp stream end: 0x%X (%d)" % (end, end))


if __name__ == "__main__":
    main()
