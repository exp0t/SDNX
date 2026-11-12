"""Xenos shader containers of Sleeping Dogs: parse the header the same way port/tools/shaderlib/ucode_hash.c
does, show the microcode, and (with --make) build a container for a raw microcode blob so the offline
translator (XenosRecomp -> HLSL -> DXC -> SPIR-V) can be run on shaders that the game creates internally.

Used for the run-time "internal D3D" vertex shader (key B6C9863F710683EC, 24 words, no fetches).
"""
import argparse
import struct
import sys


def be32(d, o):
    return (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]


def parse(path):
    d = open(path, "rb").read()
    magic = be32(d, 0)
    virt = be32(d, 4)
    hdr = be32(d, 24)
    start = virt + be32(d, hdr)
    size = be32(d, hdr + 4)
    return d, dict(magic=magic, virt=virt, hdr=hdr, start=start, size=size, total=len(d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--words", action="store_true", help="print the microcode words")
    ap.add_argument("--make", metavar="TEMPLATE", help="write a container made from TEMPLATE with the microcode of PATH")
    ap.add_argument("--out")
    args = ap.parse_args()

    if args.make:
        # args.path is a raw microcode blob (no container header): wrap it into a copy of TEMPLATE.
        code = open(args.path, "rb").read()
        t, tf = parse(args.make)
        print(f"template {args.make}: magic=0x{tf['magic']:08X} start={tf['start']} size={tf['size']} total={tf['total']}")
        words = len(code) // 4
        print(f"raw microcode: {len(code)} bytes, {words} words")
        out = bytearray(t)
        hdr = tf["hdr"]
        struct.pack_into(">I", out, hdr + 4, len(code))
        struct.pack_into(">I", out, 8, words)
        # Direct3D's own shaders carry no constant table: XenosRecomp then links vertex exports to pixel
        # inputs by register (export N -> TEXCOORDN), which is what these internal shaders rely on.
        struct.pack_into(">I", out, 0x10, 0)  # ShaderContainer::constantTableOffset = NULL
        struct.pack_into(">I", out, hdr + 0x14, 0)  # Shader::interpolatorInfo (no declared interpolators)
        start = tf["start"]
        out[start:start + len(code)] = code
        del out[start + len(code):]
        dest = args.out or ("made_" + args.path.rsplit("\\", 1)[-1])
        open(dest, "wb").write(bytes(out))
        print(f"written {dest}: {len(out)} bytes")
        return

    d, f = parse(args.path)
    for k, v in f.items():
        print(f"{k:6} = {v} (0x{v:X})" if isinstance(v, int) else f"{k} = {v}")
    if f["start"] + f["size"] > f["total"]:
        print("WARNING: microcode range exceeds the file")
    code = d[f["start"]:f["start"] + f["size"]]
    print(f"microcode: {len(code)} bytes, {len(code)//4} words")
    if args.words:
        for i in range(0, len(code) // 4):
            print(f"  [{i:3}] {be32(code, i*4):08X}")

    if args.make:
        t, tf = parse(args.make)
        words = len(code) // 4
        out = bytearray(t)
        hdr = tf["hdr"]
        struct.pack_into(">I", out, hdr + 4, len(code))
        # dword 8 of the header is the microcode size in dwords for these containers.
        struct.pack_into(">I", out, 8, words)
        start = tf["start"]
        out[start:start + len(code)] = code
        del out[start + len(code):]
        dest = args.out or ("made_" + args.path.rsplit("\\", 1)[-1])
        open(dest, "wb").write(bytes(out))
        print(f"written {dest}: {len(out)} bytes")


main()
