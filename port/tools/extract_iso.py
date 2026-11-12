#!/usr/bin/env python3
"""Minimal XDVDFS extractor (Xbox 360 XGD2/XGD3 ISO), linear-scan directory tables.

Usage:
  python extract_iso.py <iso> list
  python extract_iso.py <iso> extract <outdir> [--only NAME ...]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def find_partition_base(fh, limit=128 * 1024 * 1024):
    fh.seek(0)
    off = 0
    chunk = 1 << 20
    while off < limit:
        data = fh.read(chunk)
        if not data:
            break
        i = data.find(MAGIC)
        if i >= 0:
            return off + i - 32 * SECTOR
        off += len(data)
    raise SystemExit("XDVDFS volume not found")


def read_at(fh, off, size):
    fh.seek(off)
    return fh.read(size)


def parse_table(buf, table_sector):
    """Linear scan of one directory table. Returns [(name, sector, size, attrs)]."""
    out = []
    off = 0
    n = len(buf)
    while off + 14 <= n:
        left, right, sector, size, attr, name_len = struct.unpack_from("<HHIIBB", buf, off)
        if name_len == 0 or name_len > 255 or off + 14 + name_len > n:
            off += 4
            continue
        raw = buf[off + 14: off + 14 + name_len]
        if any(c < 0x20 for c in raw) or b"/" in raw:
            off += 4
            continue
        name = raw.decode("latin-1")
        out.append((name, sector, size, attr))
        off += 14 + name_len
        off += (4 - (off % 4)) % 4
        _ = (left, right, table_sector)
    return out


def walk(fh, base, sector, size, prefix, acc, depth=0):
    if depth > 16 or size == 0:
        return
    span = max(size, SECTOR)
    buf = read_at(fh, base + sector * SECTOR, span)
    if not buf:
        return
    for name, fsector, fsize, attr in parse_table(buf, sector):
        if name in (".", ".."):
            continue
        path = f"{prefix}/{name}"
        if attr & 0x10:
            acc.append((path, fsector, fsize, True))
            walk(fh, base, fsector, fsize, path, acc, depth + 1)
        else:
            acc.append((path, fsector, fsize, False))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    iso, cmd = sys.argv[1], sys.argv[2]
    fh = open(iso, "rb", buffering=0)
    base = find_partition_base(fh)
    vd = read_at(fh, base + 32 * SECTOR, SECTOR)
    root_sector, root_size = struct.unpack_from("<II", vd, 20)
    entries = []
    walk(fh, base, root_sector, root_size, "", entries)

    if cmd == "list":
        for path, sector, size, is_dir in entries:
            print(f"{'d' if is_dir else '-'} {size:>13} {path}")
        print(f"\n{len(entries)} entries, partition base 0x{base:X}")
        return

    if cmd != "extract":
        raise SystemExit("unknown command")
    outdir = sys.argv[3]
    only = set()
    if "--only" in sys.argv:
        i = sys.argv.index("--only")
        only = set(sys.argv[i + 1:])
    files = [(p, s, z) for p, s, z, d in entries if not d]
    total = sum(z for _, _, z in files)
    print(f"partition base 0x{base:X}; {len(files)} files, {total/1e9:.2f} GB")
    done = 0
    for path, sector, size in files:
        rel = path.lstrip("/")
        if only and rel not in only:
            continue
        dest = os.path.join(outdir, *rel.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as out:
            remaining = size
            pos = base + sector * SECTOR
            while remaining > 0:
                chunk = read_at(fh, pos, min(8 << 20, remaining))
                if not chunk:
                    raise SystemExit(f"short read on {path}")
                out.write(chunk)
                pos += len(chunk)
                remaining -= len(chunk)
        done += size
        print(f"  {rel} ({size/1e6:.1f} MB)  [{done/1e9:.2f}/{total/1e9:.2f} GB]", flush=True)


if __name__ == "__main__":
    main()
