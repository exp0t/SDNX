#!/usr/bin/env python3
"""Rebuild every localisation table of the given archives and compare byte for byte with the original."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdbig  # noqa: E402
import sdtext  # noqa: E402

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")

for arc in sys.argv[1:] or ["UI", "SeekFreeBoot", "Global"]:
    bi = sdbig.BigIndex(os.path.join(ASSETS, arc))
    ok = bad = 0
    names = set()
    with open(bi.big_path, "rb") as f:
        for e in bi.entries:
            d = bi.read(e, f)
            for p, ln in sdtext.iter_text_chunks(d):
                t = sdtext.TextTable(d[p:p + ln])
                names.add(t.name.split("_", 1)[0])
                if t.build() == d[p:p + ln]:
                    ok += 1
                else:
                    bad += 1
                    print("BAD", arc, "%08X" % e.uid, t.name, ln, len(t.build()))
    print(arc, "ok", ok, "bad", bad, "langs", sorted(names))
