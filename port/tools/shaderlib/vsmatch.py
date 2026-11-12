"""Match run-time vertex shader microcode (SDK dump, SDNX_DUMP_UCODE) to Xenos containers.

Direct3D rewrites the vertex fetch instructions of a vertex shader at bind (destination swizzle, format,
stride, offset, prefetch / mini-fetch flags). The fetch instructions are found exactly by parsing the
control flow (exec clauses, sequence bit 0 = fetch, word 0 opcode 0 = vertex fetch); every other instruction
must match bit for bit, and in vertex fetches the opcode, source and destination registers and constant index
(word 0 bits 0-26) must match. Prints how many run-time shaders have a candidate and whether the candidates
are interchangeable for translation.
Usage: vsmatch.py <containers dir> <runtime dir> [variants out dir]
"""
import collections
import glob
import os
import struct
import sys


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}


def vfetch_addresses(w):
    """Instruction addresses (units of 3 words) of the vertex fetches, from the control flow."""
    out = set()
    first_instr = None
    i = 0
    while True:
        if first_instr is not None and i * 3 >= first_instr * 3:
            break
        if i * 3 + 2 >= len(w):
            break
        a, b, c = w[i * 3:i * 3 + 3]
        for cf in (a | ((b & 0xFFFF) << 32), (b >> 16) | (c << 16)):
            op = (cf >> 44) & 0xF
            if op in EXEC_OPS:
                addr, count, seq = cf & 0xFFF, (cf >> 12) & 7, (cf >> 16) & 0xFFF
                if first_instr is None or addr < first_instr:
                    first_instr = addr
                for k in range(count):
                    if (seq >> (2 * k)) & 1:
                        g = (addr + k) * 3
                        if g + 2 < len(w) and (w[g] & 0x1F) == 0:
                            out.add(addr + k)
        i += 1
    return out


def micro(d):
    virt, hdr = be32(d, 4), be32(d, 24)
    st, sz = virt + be32(d, hdr), be32(d, hdr + 4)
    return st, sz, struct.unpack(">%dI" % (sz // 4), d[st:st + sz])


def matches(cw, fetches, w, word0_mask):
    if len(cw) != len(w):
        return False
    for g in range(0, len(w), 3):
        a, b = cw[g:g + 3], w[g:g + 3]
        if a == b:
            continue
        if g // 3 not in fetches:
            return False
        if (a[0] & word0_mask) != (b[0] & word0_mask):
            return False
    return True


def main(cdir, rdir, vout=None):
    lib = collections.defaultdict(list)
    for p in glob.glob(os.path.join(cdir, "vs_*.bin")):
        d = open(p, "rb").read()
        st, sz, w = micro(d)
        lib[len(w)].append((os.path.basename(p)[:-4], w, vfetch_addresses(w), d, st, sz))
    stats = collections.Counter()
    pairs = []
    for p in sorted(glob.glob(os.path.join(rdir, "*.ucode.bin.vert"))):
        r = open(p, "rb").read()
        n = len(r) // 4
        w = struct.unpack("<%dI" % n, r)
        h = os.path.basename(p).split("_")[1].split(".")[0]
        strict = [x for x in lib[n] if matches(x[1], x[2], w, (1 << 27) - 1)]
        regs = [x for x in lib[n] if matches(x[1], x[2], w, (1 << 20) - 1)]
        loose = [x for x in lib[n] if matches(x[1], x[2], w, 0x1F)]
        m = strict or regs or loose
        stats["strict" if strict else "regs" if regs else "loose" if loose else "none"] += 1
        if m:
            name, cw, fetches, d, st, sz = m[0]
            pairs.append((h, name))
            if vout:
                dd = bytearray(d)
                dd[st:st + sz] = struct.pack(">%dI" % n, *w)
                open(os.path.join(vout, "vs_%s.bin" % h), "wb").write(dd)
    print(dict(stats))
    if vout:
        with open(os.path.join(vout, "pairs.txt"), "w", newline="\n") as f:
            f.write("".join("%s %s\n" % x for x in pairs))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
