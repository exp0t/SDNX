import struct

# Microcode of the run-time "internal D3D" pixel shader (key A4A965C189287B99), host-order words as the
# renderer logged them (SDNX_TRACE_PS), 9 words.
words = [
    0x00000000, 0x1001C400, 0x22000000,
    0xC80F8000, 0x00000000, 0xE2000000,
    0x00000000, 0x00000000, 0x00000000,
]

out = b"".join(struct.pack(">I", w) for w in words)
path = r"D:\CODE\SDNX\port\shaders\internal_ps.bin"
open(path, "wb").write(out)
print(f"wrote {path}: {len(out)} bytes ({len(words)} words)")
