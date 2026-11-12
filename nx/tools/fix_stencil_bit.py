#!/usr/bin/env python3
"""ReXGlue / masseffect-nx SDK (and upstream Xenia): Vulkan depth -> stencil-bit EDRAM transfers never discard.

Without VK_EXT_shader_stencil_export (NVIDIA on Windows, NVK 26.x on the Switch) stencil is transferred by 8 draws,
one per bit, each killing the fragment when that bit of the source stencil is 0. The transfer shader generator only
fills `packed` (the value the kill test reads) inside the "source depth loaded" branch, but kDepthToStencilBit
does not load depth at all, so `packed` stays empty, the kill is never emitted and every bit is written: after
any depth transfer without stencil export the destination stencil is 0xFF everywhere.

Sleeping Dogs: the game clears stencil to 0 in a 4x MSAA view of the depth buffer, switches back to 1x (transfer)
and draws its full-screen scene composite with stencil == 0. With stencil 0xFF the pass draws nothing, and the
picture left in EDRAM is the 7e3 HDR scene bits reinterpreted as 8888: "psychedelic" colours. Found 2026-11-08
in RenderDoc (frame_frame14618, transfer ps 1297 has no OpKill). Fix: take the stencil directly. Idempotent.

Usage: fix_stencil_bit.py <sdk root>
"""
import re
import sys

path = sys.argv[1].rstrip("/\\") + "/src/graphics/vulkan/render_target_cache.cpp"
s = open(path, encoding="utf-8").read().replace("\r\n", "\n")
mark = "SDNX: depth -> stencil bit"
if mark in s:
    print("already fixed:", path)
    sys.exit(0)
pat = re.compile(r"(builder\.makeUintConstant\(8\),\s*builder\.makeUintConstant\(24\)\);\n\s*\}\n\s*\}\n)(\s*)\}\n(\s*switch \(mode\.output\) \{)")
m = pat.search(s)
if not m:
    sys.exit("pattern not found: " + path)
ind = m.group(2)
add = (ind + "} else if (mode.output == TransferOutput::kStencilBit && source_stencil[0] != spv::NoResult) {\n" +
       ind + "  // " + mark + " transfers don't load depth, the kill test needs the stencil in 0:7.\n" +
       ind + "  packed = source_stencil[0];\n" +
       ind + "}\n")
s = s[:m.start()] + m.group(1) + add + m.group(3) + s[m.end():]
open(path, "w", encoding="utf-8", newline="\n").write(s)
print("fixed:", path)
