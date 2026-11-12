import struct

# Microcode of the run-time "internal D3D" vertex shader (key B6C9863F710683EC), host-order words as the
# renderer logged them (SDNX_TRACE_VS), 24 words.
words = [
    0x00001003, 0x00001200, 0xC4000000,
    0x00001004, 0x00001200, 0xC2000000,
    0x00001005, 0x10061200, 0x22000000,
    0xC8000000, 0x00000000, 0xE2000000,
    0xC800C000, 0x00000000, 0xE2000000,
    0xC888C03E, 0x00000000, 0xE2010100,
    0xC8000000, 0x00000000, 0xE2000000,
    0x00000000, 0x00000000, 0x00000000,
]

out = b"".join(struct.pack(">I", w) for w in words)
path = r"D:\CODE\SDNX\port\shaders\internal_vs.bin"
open(path, "wb").write(out)
print(f"wrote {path}: {len(out)} bytes ({len(words)} words)")
