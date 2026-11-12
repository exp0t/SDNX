"""Sleeping Dogs (X360): extract every Xenos shader container (signature 0x102A1100 pixel / 0x102A1101 vertex)
from the game's .big archives, for the offline shader library of the native renderer (docs/native-renderer-plan.md).

Each archive entry is decompressed (sdbig.BigIndex) and scanned at 4-byte alignment for the signature. A hit is
kept only if it passes the layout checks the masseffect-nx runtime makes (shaders/tools/container_check.h):
shader header and microcode inside the container. Size = virtualSize + physicalSize. Output, deduplicated by
content: <out>/vs_<sha1:16>.bin, <out>/ps_<sha1:16>.bin, and <out>/containers.tsv (name, archive, entry, offset).
Nothing from the game is meant to leave the user's machine.

Usage: sdshaders.py <game_root> <out_dir>
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sdbig import BigIndex  # noqa: E402


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def container_at(d, o):
    """Container bytes at offset o, or None if the layout checks fail."""
    if o + 36 > len(d):
        return None
    sig = be32(d, o)
    vertex = sig & 1
    virt, phys = be32(d, o + 4), be32(d, o + 8)
    size = virt + phys
    if size < 36 or o + size > len(d) or size > 4 << 20:
        return None
    c = d[o:o + size]
    header = be32(c, 24)
    if header + 8 > size:
        return None
    micro_start = virt + be32(c, header)
    micro_size = be32(c, header + 4)
    if micro_size == 0 or micro_size % 4 or micro_start + micro_size > size:
        return None
    if header >= virt or header + (40 if vertex else 32) > virt:
        return None
    return c


def main(root, out):
    os.makedirs(out, exist_ok=True)
    seen = {}
    rows = []
    hits = 0
    for name in sorted(os.listdir(root)):
        if not name.endswith(".bix") or name.endswith(".orig"):
            continue
        base = os.path.join(root, name[:-4])
        try:
            bi = BigIndex(base)
        except Exception as ex:  # noqa: BLE001
            print("skip %s: %s" % (name, ex))
            continue
        n_arch = 0
        with open(bi.big_path, "rb") as f:
            for e in bi.entries:
                try:
                    d = bi.read(e, f)
                except Exception:  # noqa: BLE001
                    continue
                o = d.find(b"\x10\x2A\x11")
                while o >= 0:
                    if o % 4 == 0 and d[o + 3] in (0, 1):
                        c = container_at(d, o)
                        if c is not None:
                            hits += 1
                            n_arch += 1
                            kind = "vs" if c[3] & 1 else "ps"
                            fn = "%s_%s.bin" % (kind, hashlib.sha1(c).hexdigest()[:16])
                            if fn not in seen:
                                seen[fn] = len(c)
                                open(os.path.join(out, fn), "wb").write(c)
                            rows.append("%s\t%s\t%08X\t%X" % (fn, name[:-4], e.uid, o))
                    o = d.find(b"\x10\x2A\x11", o + 1)
        print("%-14s %6d containers" % (name[:-4], n_arch))
    with open(os.path.join(out, "containers.tsv"), "w") as f:
        f.write("\n".join(rows) + "\n")
    vs = sum(1 for k in seen if k.startswith("vs"))
    print("total hits %d, distinct %d (vs %d, ps %d) -> %s" % (hits, len(seen), vs, len(seen) - vs, out))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
