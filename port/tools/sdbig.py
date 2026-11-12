#!/usr/bin/env python3
"""Sleeping Dogs (Xbox 360) BIG/BIX archive tool.

Format (checked on the retail X360 data, see docs/branding.md):

  NAME.bix  one qChunk (big-endian):  u32 chunk_uid, u32 size, u32 data_size, u32 data_offset,
            then BIGFileIndex (32-bit build): qResourceData header (0x40 bytes, name at +0x18),
            s64 sort_key, u32 entry_count, u32 rel_offset_to_entries (relative to its own field),
            u32 file, u16 state, u16 mount, u32 pad0, u32 pad1, char big_name[32], entries...
  entry     u32 uid, u32 offset_div_4, u32 load_offset, u32 compressed_size, u32 compressed_extra,
            u32 uncompressed_size                                             (24 bytes, sorted by uid)
  NAME.big  data of entry = file[offset*4 + (load_offset & 0xFFF) : ...]
            compressed_size == 0  -> stored, uncompressed_size bytes
            compressed_size != 0  -> UFG LZ stream with a little-endian 'PMCQ' (QCMP) header

Usage:
  sdbig.py list   UI                       (path without extension, or .bix/.big)
  sdbig.py extract UI out_dir [uid ...]
  sdbig.py grep   UI pattern               (search decompressed entries, ASCII/UTF-8)
"""
import os
import struct
import sys

HDR_ID = 0x51434D50


# ---------------------------------------------------------------------------- UFG LZ (qDecompressLZ)

def lz_decompress(buf: bytes) -> bytes:
    """quark/compresslz.hh::qDecompressLZ. Header fields are little-endian on every platform."""
    hid, typ, ver, data_off, extra, csize, usize, chk = struct.unpack_from("<IHHIIqqq", buf, 0)
    if hid != HDR_ID:
        raise ValueError("not a QCMP stream (id %08X)" % hid)
    out = bytearray()
    cache = [(0, 0)] * 32
    cur = 0
    i = data_off
    end = csize
    while i < end:
        bits = buf[i]
        i += 1
        if bits < 32:
            n = bits + 1
            out += buf[i:i + n]
            i += n
            continue
        idx = bits & 31
        et = bits >> 5
        if et == 1:
            off, ln = cache[idx]
        else:
            off = buf[i] | (idx << 8)
            i += 1
            if et == 7:
                ln = buf[i] + 1
                i += 1
            else:
                ln = et + 1
            cache[cur] = (off, ln)
            cur = (cur + 1) % 32
        s = len(out) - off
        for k in range(ln):  # overlapping copy
            out.append(out[s + k])
    if len(out) != usize:
        raise ValueError("size mismatch %d != %d" % (len(out), usize))
    return bytes(out)


# ---------------------------------------------------------------------------- index

class Entry:
    __slots__ = ("index", "uid", "off4", "load_offset", "csize", "cextra", "usize", "pos")

    def __init__(self, index, pos, uid, off4, load_offset, csize, cextra, usize):
        self.index, self.pos = index, pos
        self.uid, self.off4, self.load_offset = uid, off4, load_offset
        self.csize, self.cextra, self.usize = csize, cextra, usize

    @property
    def data_pos(self):
        return self.off4 * 4 + (self.load_offset & 0xFFF)

    @property
    def stored_size(self):
        return self.csize if self.csize else self.usize


class BigIndex:
    ENTRY_FMT = ">IIIIII"

    def __init__(self, base):
        for ext in (".bix", ".big"):
            if base.lower().endswith(ext):
                base = base[: -len(ext)]
        self.base = base
        self.bix_path = base + ".bix"
        self.big_path = base + ".big"
        self.raw = bytearray(open(self.bix_path, "rb").read())
        r = self.raw
        # chunk header (16) + resource header; the entry count/offset pair sits at 0x58/0x5C
        self.count, rel = struct.unpack_from(">II", r, 0x58)
        self.entries_pos = 0x5C + rel
        self.name = r[0x70:0x90].split(b"\0", 1)[0].decode("ascii", "replace")
        self.entries = []
        for n in range(self.count):
            p = self.entries_pos + n * 24
            self.entries.append(Entry(n, p, *struct.unpack_from(self.ENTRY_FMT, r, p)))
        self.by_uid = {e.uid: e for e in self.entries}

    def read(self, e: Entry, f=None) -> bytes:
        own = f is None
        if own:
            f = open(self.big_path, "rb")
        try:
            f.seek(e.data_pos)
            d = f.read(e.stored_size)
        finally:
            if own:
                f.close()
        return lz_decompress(d) if e.csize else d

    def write_entry(self, e: Entry):
        struct.pack_into(self.ENTRY_FMT, self.raw, e.pos, e.uid, e.off4, e.load_offset, e.csize,
                         e.cextra, e.usize)

    def save_bix(self, path=None):
        open(path or self.bix_path, "wb").write(self.raw)


# ---------------------------------------------------------------------------- perm-file chunks

def iter_chunks(data: bytes):
    """Top-level qChunks of a .perm resource stream: u32 uid, u32 size, u32 data_size, u32 data_off."""
    p = 0
    n = len(data)
    while p + 16 <= n:
        uid, size, dsize, doff = struct.unpack_from(">IIII", data, p)
        if size == 0 and uid == 0:
            break
        yield p, uid, size, dsize, doff
        p += 16 + size
        if size == 0:
            break


def chunk_name(data: bytes, p: int) -> str:
    """Debug name of the resource in a chunk (qResourceData::mDebugName at +0x1C of the payload)."""
    s = data[p + 16 + 0x1C: p + 16 + 0x1C + 36]
    s = s.split(b"\0", 1)[0]
    try:
        t = s.decode("ascii")
    except UnicodeDecodeError:
        return ""
    return t if t.isprintable() else ""


# ---------------------------------------------------------------------------- CLI

def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, base = argv[1], argv[2]
    bi = BigIndex(base)
    if cmd == "list":
        print("%s: %d entries, name %r" % (bi.bix_path, bi.count, bi.name))
        with open(bi.big_path, "rb") as f:
            for e in bi.entries:
                name = ""
                try:
                    d = bi.read(e, f)
                    for p, uid, size, dsize, doff in iter_chunks(d):
                        name = chunk_name(d, p)
                        if name:
                            break
                except Exception as ex:  # noqa: BLE001
                    name = "<%s>" % ex
                print("%08X @%08X c=%7d u=%7d %s" % (e.uid, e.data_pos, e.csize, e.usize, name))
    elif cmd == "chunks":
        # every top-level chunk of every entry: entry uid, chunk offset, chunk type uid, size, name
        with open(bi.big_path, "rb") as f:
            for e in bi.entries:
                try:
                    d = bi.read(e, f)
                except Exception as ex:  # noqa: BLE001
                    print("%08X <%s>" % (e.uid, ex))
                    continue
                for p, uid, size, dsize, doff in iter_chunks(d):
                    print("%08X +%06X %08X %7d %s" % (e.uid, p, uid, size, chunk_name(d, p)))
    elif cmd == "extract":
        out = argv[3]
        os.makedirs(out, exist_ok=True)
        want = {int(x, 16) for x in argv[4:]}
        with open(bi.big_path, "rb") as f:
            for e in bi.entries:
                if want and e.uid not in want:
                    continue
                open(os.path.join(out, "%08X.bin" % e.uid), "wb").write(bi.read(e, f))
    elif cmd == "grep":
        pat = argv[3].encode("utf-8")
        low = pat.lower()
        with open(bi.big_path, "rb") as f:
            for e in bi.entries:
                d = bi.read(e, f)
                dl = d.lower()
                k = dl.find(low)
                hits = 0
                while k >= 0:
                    hits += 1
                    k = dl.find(low, k + 1)
                if hits:
                    print("%08X hits=%d size=%d" % (e.uid, hits, len(d)))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
