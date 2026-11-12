#!/usr/bin/env python3
"""Add an "FPS LIMITER" row to the Display options screen (Scaleform movie Options_Display, UI.big 0xEBCA65F0).

The row uses the list's third slot, which in this movie is the PC resolution slot (arrows + a caption list):
captions "30", "60", "$COMMON_OFF_UPPERCASE" (the game translates it: OFF / ОТКЛ.), label "$OPTIONS_FPS_LIMITER" (FPS LIMITER / ОГРАНИЧЕНИЕ FPS — a PC string that is still
in all 7 languages). Its id is OPTIONS_FPS_LIMITER, so left/right sends FSCommand "OPTIONS_FPS_LIMITER" with the
index 0/1/2 to the screen's handleMessage (sub_827ECC70), where nx/sdretail/src/sdnx_fps_limit.cpp picks it up.
The NRO sets the shown value on open by calling SDNX_SetFpsLimit(index) (Invoke) right after the game's
OptionList_SetValues (hook on sub_827C9358).

ActionScript appended to the end of the main timeline frame script (after OptionList_PopulateData has run):

    OptionList_AddResolutionOption("OPTIONS_FPS_LIMITER", "$OPTIONS_FPS_LIMITER");
    OptionList_AddResolution("30"); OptionList_AddResolution("60"); OptionList_AddResolution("$COMMON_OFF_UPPERCASE");
    OptionList.NumSlots = 3;
    OptionList_SetResolutionIndex(0);
    function SDNX_SetFpsLimit(v) { OptionList_SetResolutionIndex(v); }
"""
import struct
import zlib

ENTRY_UID = 0xEBCA65F0
CHUNK_MOVIE = 0x442A39D9
MARKER = b"SDNX_SetFpsLimit"


# ---------------------------------------------------------------------------- tiny AS2 assembler

def _act(code, payload=b""):
    if code >= 0x80:
        return bytes([code]) + struct.pack("<H", len(payload)) + payload
    return bytes([code])


def push(*vals):
    p = b""
    for v in vals:
        if isinstance(v, str):
            p += b"\x00" + v.encode("latin-1") + b"\x00"
        elif isinstance(v, bool):
            p += b"\x05" + bytes([1 if v else 0])
        elif isinstance(v, int):
            p += b"\x07" + struct.pack("<i", v)
        else:
            raise TypeError(v)
    return _act(0x96, p)


GETVAR, SETMEMBER, CALLMETHOD, CALLFUNC, POP, GETMEMBER = (_act(0x1C), _act(0x4F), _act(0x52), _act(0x3D),
                                                           _act(0x17), _act(0x4E))


def call_function(name, *args):          # result popped
    return push(*reversed(args), len(args), name) + CALLFUNC + POP


def option_list():
    return push("OptionList") + GETVAR


def get_item(i):                         # leaves the item on the stack
    return push(i, 1, "OptionList") + GETVAR + push("GetItem") + CALLMETHOD


def define_function(name, params, body):
    p = name.encode() + b"\0" + struct.pack("<H", len(params))
    for prm in params:
        p += prm.encode() + b"\0"
    p += struct.pack("<H", len(body))
    return _act(0x9B, p) + body


def build_code():
    # The list's slot clips have fixed classes from the PC layout: slot2 = OptionResolutionSlot (arrows + a list of
    # captions), slot3/4 = Txt2, slot5 = Spinner. The third row lands in slot2, so it must be a "resolution" item:
    # a spinner item there is not accepted by the slot (it showed "undefined").
    c = b""
    c += call_function("OptionList_AddResolutionOption", "OPTIONS_FPS_LIMITER", "$OPTIONS_FPS_LIMITER")
    for cap in ("30", "60", "$COMMON_OFF_UPPERCASE"):
        c += call_function("OptionList_AddResolution", cap)
    c += option_list() + push("NumSlots", 3) + SETMEMBER
    c += call_function("OptionList_SetResolutionIndex", 0)
    body = push("v") + GETVAR + push(1, "OptionList_SetResolutionIndex") + CALLFUNC + POP
    c += define_function("SDNX_SetFpsLimit", ["v"], body)
    return c


# ---------------------------------------------------------------------------- SWF editing

def _rect_len(b):
    return (5 + 4 * (b[0] >> 3) + 7) // 8


def _tags(body, p):
    while p < len(body):
        cl = struct.unpack_from("<H", body, p)[0]
        code, ln = cl >> 6, cl & 0x3F
        hl = 2
        if ln == 0x3F:
            ln = struct.unpack_from("<I", body, p + 2)[0]
            hl = 6
        yield code, p, hl, ln
        p += hl + ln
        if code == 0:
            break


def patch_swf_body(body: bytes) -> bytes:
    """Append the code to the main-timeline DoAction that calls OptionList_PopulateData."""
    if MARKER in body:
        raise ValueError("already patched")
    p0 = _rect_len(body) + 4
    target = None
    for code, p, hl, ln in _tags(body, p0):
        if code == 12 and b"OptionList_PopulateData" in body[p + hl:p + hl + ln] and ln > 1000:
            target = (p, hl, ln)
    if not target:
        raise ValueError("frame script not found")
    p, hl, ln = target
    action = body[p + hl:p + hl + ln]
    if action[-1] != 0:
        raise ValueError("frame script does not end with End")
    new_action = action[:-1] + build_code() + b"\x00"
    tag = struct.pack("<HI", (12 << 6) | 0x3F, len(new_action)) + new_action
    return body[:p] + tag + body[p + hl + ln:]


def patch_movie(movie: bytes) -> bytes:
    sig, ver = movie[:3], movie[3]
    if sig == b"CFX":
        body = zlib.decompress(movie[8:])
    elif sig == b"GFX":
        body = movie[8:]
    else:
        raise ValueError("not a GFx movie: %r" % sig)
    nb = patch_swf_body(body)
    return b"CFX" + bytes([ver]) + struct.pack("<I", len(nb) + 8) + zlib.compress(nb, 9)


def patch_entry(data: bytes) -> bytes:
    """Entry = one chunk 0x442A39D9: header(16) + resource header(0x40) + u32 movie size + u32 4 + movie + pad16."""
    uid, size, dsize, doff = struct.unpack_from(">IIII", data, 0)
    if uid != CHUNK_MOVIE:
        raise ValueError("not a movie chunk")
    msize, rel = struct.unpack_from(">II", data, 0x50)
    movie = data[0x58:0x58 + msize]
    nm = patch_movie(movie)
    out = bytearray(data[:0x50]) + struct.pack(">II", len(nm), rel) + nm
    while len(out) % 16:
        out.append(0)
    out += data[16 + size:]                  # anything after the chunk (normally nothing)
    struct.pack_into(">II", out, 4, len(out) - 16 - (len(data) - 16 - size), len(out) - 16 - (len(data) - 16 - size))
    return bytes(out)


def is_patched(data: bytes) -> bool:
    try:
        msize = struct.unpack_from(">I", data, 0x50)[0]
        m = data[0x58:0x58 + msize]
        body = zlib.decompress(m[8:]) if m[:3] == b"CFX" else m[8:]
        return MARKER in body
    except Exception:  # noqa: BLE001
        return False
