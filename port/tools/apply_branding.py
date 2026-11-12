#!/usr/bin/env python3
"""SDNX branding patch: "Nintendo Switch" text and Switch button glyphs in the Sleeping Dogs X360 data.

Works on the user's own extracted game data (the game_root folder). Changes:
  * localisation tables (all 7 languages): the strings listed in port/branding/switch_text.json;
  * UI textures BUTTON_* (perm 0x14B0D2FB + temp 0xFBEDD2FF): Switch-style glyphs from make_switch_buttons.py;
  * Settings > Display movie (0xEBCA65F0): an "FPS LIMITER" row 30 / 60 / OFF (fps_menu_patch.py), read by the NRO.

How an archive is patched: each changed entry is written uncompressed at the end of NAME.big (4 KB aligned) and
its NAME.bix record is pointed there (compressed_size = 0, as the game's own stored entries). The original bytes
stay in place, so --restore only truncates the .big and puts back the saved .bix.

Usage:
  apply_branding.py GAME_ROOT [--by-letter] [--no-text] [--no-buttons] [--no-fps-menu]
  apply_branding.py GAME_ROOT --restore
  apply_branding.py GAME_ROOT --verify OUT_DIR      (decode patched glyphs + changed strings for review)

Face buttons are labelled by position by default (Xbox A = bottom = Switch B, B -> A, X -> Y, Y -> X), matching
the NRO's default input mapping (input_xbox_layout = true). --by-letter: labels A/B/X/Y as on the Xbox, for
input_xbox_layout = false.
"""
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402

import fps_menu_patch  # noqa: E402
import sdbig  # noqa: E402
import sdtex  # noqa: E402
import sdtext  # noqa: E402
import xtex  # noqa: E402

ARCHIVES = ["UI", "SeekFreeBoot"]
BUTTONS_PERM = 0x14B0D2FB
BUTTONS_TEMP = 0xFBEDD2FF
TEXT_JSON = os.path.join(HERE, "..", "branding", "switch_text.json")
STATE_SUFFIX = ".sdnx_branding.json"
ALIGN = 0x1000


def load_text_table():
    raw = json.load(open(TEXT_JSON, encoding="utf-8"))
    out = {}
    for k, v in raw.items():
        if k.startswith("_"):
            continue
        out[int(k, 16)] = {lang: s.encode("utf-8") for lang, s in v.items() if not lang.startswith("_")}
    return out


# ---------------------------------------------------------------------------- state / restore

def state_path(bi):
    return bi.base + STATE_SUFFIX


def restore(base, quiet=False):
    bi_base = base
    st = bi_base + STATE_SUFFIX
    if not os.path.exists(st):
        if not quiet:
            print("  %s: not patched" % os.path.basename(base))
        return False
    s = json.load(open(st))
    with open(bi_base + ".big", "r+b") as f:
        f.truncate(s["big_size"])
    shutil.copyfile(bi_base + ".bix.orig", bi_base + ".bix")
    os.remove(bi_base + ".bix.orig")
    os.remove(st)
    print("  %s: restored (big %d bytes)" % (os.path.basename(base), s["big_size"]))
    return True


# ---------------------------------------------------------------------------- patchers

def patch_text(data: bytes, table) -> (bytes, list):
    """Rebuild every localisation chunk of an entry that contains one of the listed hashes."""
    chunks = list(sdtext.iter_text_chunks(data))
    if not chunks:
        return None, []
    out = bytearray()
    last = 0
    changed = []
    for p, ln in chunks:
        t = sdtext.TextTable(data[p:p + ln])
        lang = t.name.split("_", 1)[0]
        hit = False
        for ent in t.entries:
            rep = table.get(ent[0], {}).get(lang)
            if rep is not None and ent[1] != rep:
                changed.append((t.name, ent[0]))
                ent[1] = rep
                hit = True
        out += data[last:p]
        out += t.build() if hit else data[p:p + ln]
        last = p + ln
    out += data[last:]
    return (bytes(out) if changed else None), changed


def patch_buttons(perm: bytes, temp: bytes, glyphs) -> (bytes, list):
    temp = bytearray(temp)
    done = []
    for t in sdtex.iter_textures(perm):
        img = glyphs.get(t["name"])
        if img is None:
            continue
        if sdtex.FORMATS.get(t["fmt"]) != "DXT5" or img.shape[1] != t["w"] or img.shape[0] != t["h"]:
            raise SystemExit("unexpected texture %s: fmt %d %dx%d" % (t["name"], t["fmt"], t["w"], t["h"]))
        wb, hb = t["w"] // 4, t["h"] // 4
        orig = xtex.swap16(bytes(temp[t["pos"]:t["pos"] + t["size"]]))
        lin = xtex.encode_dxt5(img)
        tiled = xtex.tile_blocks(lin, wb, hb, 16, t["size"], base=orig)
        temp[t["pos"]:t["pos"] + t["size"]] = xtex.swap16(tiled)
        done.append(t["name"])
    return bytes(temp), done


# ---------------------------------------------------------------------------- archive writer

def append_entries(bi: sdbig.BigIndex, new_data: dict):
    with open(bi.big_path, "r+b") as f:
        f.seek(0, 2)
        end = f.tell()
        for uid, data in sorted(new_data.items()):
            pos = (end + ALIGN - 1) & ~(ALIGN - 1)
            f.seek(end)
            f.write(b"\0" * (pos - end))
            f.write(data)
            end = pos + len(data)
            e = bi.by_uid[uid]
            e.off4, e.load_offset, e.csize, e.cextra, e.usize = pos // 4, 0, 0, 0, len(data)
            bi.write_entry(e)
    bi.save_bix()


def patch_archive(base, table, glyphs, fps_menu=True):
    restore(base, quiet=True)
    bi = sdbig.BigIndex(base)
    big_size = os.path.getsize(bi.big_path)
    new_data = {}
    text_changes = []
    with open(bi.big_path, "rb") as f:
        if table:
            for e in bi.entries:
                d = bi.read(e, f)
                nd, ch = patch_text(d, table)
                if nd is not None:
                    new_data[e.uid] = nd
                    text_changes += ch
        btn = []
        if glyphs and BUTTONS_PERM in bi.by_uid and BUTTONS_TEMP in bi.by_uid:
            perm = bi.read(bi.by_uid[BUTTONS_PERM], f)
            temp = bi.read(bi.by_uid[BUTTONS_TEMP], f)
            nt, btn = patch_buttons(perm, temp, glyphs)
            new_data[BUTTONS_TEMP] = nt
        fps = False
        if fps_menu and fps_menu_patch.ENTRY_UID in bi.by_uid:
            d = bi.read(bi.by_uid[fps_menu_patch.ENTRY_UID], f)
            if not fps_menu_patch.is_patched(d):
                new_data[fps_menu_patch.ENTRY_UID] = fps_menu_patch.patch_entry(d)
                fps = True
    if not new_data:
        print("  %s: nothing to change" % os.path.basename(base))
        return
    shutil.copyfile(bi.bix_path, bi.bix_path + ".orig")
    json.dump({"big_size": big_size, "entries": ["%08X" % u for u in sorted(new_data)]},
              open(state_path(bi), "w"), indent=1)
    append_entries(bi, new_data)
    langs = sorted({n.split("_", 1)[0] for n, _ in text_changes})
    print("  %s: %d strings (%s), %d glyphs, fps menu %s, %d entries rewritten, +%d KB" % (
        os.path.basename(base), len(text_changes), ", ".join(langs), len(btn), "yes" if fps else "no", len(new_data),
        (os.path.getsize(bi.big_path) - big_size) // 1024))


def verify(root, out):
    from PIL import Image
    os.makedirs(out, exist_ok=True)
    table = load_text_table()
    for arc in ARCHIVES:
        base = os.path.join(root, arc)
        if not os.path.exists(base + ".bix"):
            continue
        bi = sdbig.BigIndex(base)
        with open(bi.big_path, "rb") as f, open(os.path.join(out, arc + "_strings.txt"), "w", encoding="utf-8") as tf:
            for e in bi.entries:
                d = bi.read(e, f)
                for p, ln in sdtext.iter_text_chunks(d):
                    t = sdtext.TextTable(d[p:p + ln])
                    for h, s in t.entries:
                        if h in table or b"xbox" in s.lower():
                            tf.write("%s %08X %s\n" % (t.name, h, s.decode("utf-8", "replace")))
            if BUTTONS_PERM in bi.by_uid:
                perm = bi.read(bi.by_uid[BUTTONS_PERM], f)
                temp = bi.read(bi.by_uid[BUTTONS_TEMP], f)
                tiles = []
                for t in sdtex.iter_textures(perm):
                    if not t["name"].startswith("BUTTON"):
                        continue
                    raw = xtex.swap16(temp[t["pos"]:t["pos"] + t["size"]])
                    lin = xtex.untile_blocks(raw, t["w"] // 4, t["h"] // 4, 16)
                    tiles.append(xtex.decode_dxt(lin, t["w"], t["h"], "DXT5"))
                cell, cols = 136, 8
                rows = (len(tiles) + cols - 1) // cols
                sheet = Image.new("RGBA", (cols * cell, rows * cell), (90, 90, 90, 255))
                for i, im in enumerate(tiles):
                    sheet.alpha_composite(Image.fromarray(im, "RGBA"), ((i % cols) * cell + 4, (i // cols) * cell + 4))
                sheet.save(os.path.join(out, arc + "_buttons.png"))
    print("verify output in", out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    root = argv[1]
    if "--restore" in argv:
        for arc in ARCHIVES:
            if os.path.exists(os.path.join(root, arc + ".bix")):
                restore(os.path.join(root, arc))
        return 0
    if "--verify" in argv:
        verify(root, argv[argv.index("--verify") + 1])
        return 0
    table = None if "--no-text" in argv else load_text_table()
    glyphs = None
    if "--no-buttons" not in argv:
        import make_switch_buttons
        glyphs = make_switch_buttons.build("--by-letter" in argv)
    for arc in ARCHIVES:
        base = os.path.join(root, arc)
        if os.path.exists(base + ".bix"):
            patch_archive(base, table, glyphs, "--no-fps-menu" not in argv)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
