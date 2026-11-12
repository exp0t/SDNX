#!/usr/bin/env python3
"""ReXGlue / masseffect-nx SDK: execute_unclipped_draw_vs_on_cpu defaults to false (Xenia: true). Set it to true.

Why it matters for Sleeping Dogs: the game clears stencil with a full-screen triangle drawn without clipping and
with the full 8192x8192 scissor. Without running its vertex shader on the CPU, the render target cache assumes the
draw covers all 8192 rows; that wraps around the 2048-tile EDRAM and takes ownership of the tiles of the light
accumulation buffer. The following resolve of the light buffer then dumps the 4x MSAA depth buffer instead
(texture full of (0, 1, 1, 0)): blue picture, garbage on the floor, no proper lighting. Found 2026-11-08 with
SDNX_LOG_RESOLVE logging in the Vulkan render target cache. Idempotent.

Usage: fix_draw_extent.py <sdk root>
"""
import sys

path = sys.argv[1].rstrip("/\\") + "/src/graphics/util/draw_extent_estimator.cpp"
s = open(path, encoding="utf-8").read().replace("\r\n", "\n")
bad = "REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, false, \"GPU\","
good = "REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, true, \"GPU\","
if good in s:
    print("already fixed:", path)
elif bad in s:
    open(path, "w", encoding="utf-8", newline="\n").write(s.replace(bad, good))
    print("fixed:", path)
else:
    sys.exit("pattern not found: " + path)
