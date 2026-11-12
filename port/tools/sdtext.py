#!/usr/bin/env python3
"""Sleeping Dogs X360 localisation tables (chunk type 0x90CE6B7A, names like 'russian_Global').

Chunk layout (big-endian, offsets from the chunk start):
  +0x00 qChunk {uid 0x90CE6B7A, size, data_size, data_offset}      size = total - 16
  +0x10 qResourceData header (0x40 bytes, debug name at +0x2C)
  +0x50 u32 count
  +0x54 u32 rel  -> entries at 0x54 + rel (always 0x58)
  entries: {u32 hash, u32 rel} sorted by hash; text at (&rel) + rel, zero-terminated UTF-8,
           each string zero-padded so the next one starts on a multiple of 4

Usage:
  sdtext.py dump  ARCHIVE [pattern]     every string of every table (optionally filtered, case-insensitive)
"""
import struct
import sys

CHUNK_TEXT = 0x90CE6B7A


class TextTable:
    def __init__(self, chunk: bytes):
        uid, size, dsize, doff = struct.unpack_from(">IIII", chunk, 0)
        if uid != CHUNK_TEXT:
            raise ValueError("not a text chunk")
        self.header = chunk[:0x50]
        self.name = chunk[0x2C:0x50].split(b"\0", 1)[0].decode("latin-1")
        count, rel = struct.unpack_from(">II", chunk, 0x50)
        eo = 0x54 + rel
        self.entries = []  # [hash, bytes]
        for i in range(count):
            h, r = struct.unpack_from(">II", chunk, eo + i * 8)
            p = eo + i * 8 + 4 + r
            e = chunk.index(b"\0", p)
            self.entries.append([h, chunk[p:e]])
        self.orig_size = 16 + size

    def build(self) -> bytes:
        n = len(self.entries)
        eo = 0x58
        strings_at = eo + n * 8
        table = bytearray()
        blob = bytearray()
        for i, (h, s) in enumerate(self.entries):
            field = eo + i * 8 + 4
            table += struct.pack(">II", h, strings_at + len(blob) - field)
            blob += s + b"\0"
            while (strings_at + len(blob)) % 4:
                blob.append(0)
        body = bytearray(self.header) + struct.pack(">II", n, 4) + table + blob
        while len(body) % 4:
            body.append(0)
        struct.pack_into(">II", body, 4, len(body) - 16, len(body) - 16)
        return bytes(body)


def iter_text_chunks(data: bytes):
    p = 0
    while p + 16 <= len(data):
        uid, size, dsize, doff = struct.unpack_from(">IIII", data, p)
        if uid == 0 and size == 0:
            break
        if uid == CHUNK_TEXT:
            yield p, 16 + size
        p += 16 + size


def main(argv):
    import sdbig
    if len(argv) < 3 or argv[1] != "dump":
        print(__doc__)
        return 1
    bi = sdbig.BigIndex(argv[2])
    pat = argv[3].lower() if len(argv) > 3 else None
    out = sys.stdout.buffer
    with open(bi.big_path, "rb") as f:
        for e in bi.entries:
            d = bi.read(e, f)
            for p, ln in iter_text_chunks(d):
                t = TextTable(d[p:p + ln])
                for h, s in t.entries:
                    u = s.decode("utf-8", "replace")
                    if pat and pat not in u.lower():
                        continue
                    out.write(("%08X %s %08X\t%s\n" % (e.uid, t.name, h, u.replace("\n", "\\n"))).encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
