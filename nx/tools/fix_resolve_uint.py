#!/usr/bin/env python3
"""ReXGlue / masseffect-nx SDK (and upstream Xenia): resolves to an integer number format saturate.

Xenos resolves honour RB_COPY_DEST_INFO.copy_dest_number: with kUnsignedInteger the colour is multiplied by
2^copy_dest_exp_bias and stored as an integer. The resolve shaders always pack fixed formats as normalized
(value * 65535 for 16 bits, * 255 for 8 bits) and ignore the number format, so value * 2^bias saturates to 1.0.

Sleeping Dogs resolves its 7e3 light buffer to k_16_16_16_16 with number = unsigned integer and exp_bias = +11
(1.0 -> 2048, range 0..32) and samples it as UNORM. In Xenia every lit texel became 1.0 or 0: flat ambient-only
picture, no sunlight or sun shafts. Found 2026-11-08 (SDNX_LOG_RESOLVE: "dest_fmt=26 exp_bias=11 number=2").

Fix: for an unsigned integer destination the normalized packer gets exp_bias - 16 (16-bit) or - 8 (8-bit), i.e.
value * 2^bias / 65536 * 65535 ~ value * 2^bias (relative error 1/65536). Idempotent.

Usage: fix_resolve_uint.py <sdk root>
"""
import sys

path = sys.argv[1].rstrip("/\\") + "/src/graphics/util/draw.cpp"
s = open(path, encoding="utf-8").read().replace("\r\n", "\n")
mark = "SDNX: integer resolve destination"
if mark in s:
    print("already fixed:", path)
    sys.exit(0)
anchor = "  // Patch and write RB_COPY_DEST_INFO.\n"
if s.count(anchor) != 1:
    sys.exit("pattern not found: " + path)
add = """  // %s: the hardware stores value * 2^exp_bias as an integer, the resolve
  // shaders pack fixed formats as normalized - divide by the normalization factor (2^16 or 2^8).
  if (!is_depth &&
      rb_copy_dest_info.copy_dest_number == xenos::SurfaceNumberFormat::kUnsignedInteger) {
    switch (dest_format) {
      case xenos::TextureFormat::k_16:
      case xenos::TextureFormat::k_16_16:
      case xenos::TextureFormat::k_16_16_16_16:
        exp_bias -= 16;
        break;
      case xenos::TextureFormat::k_8:
      case xenos::TextureFormat::k_8_8:
      case xenos::TextureFormat::k_8_8_8_8:
        exp_bias -= 8;
        break;
      default:
        break;
    }
    exp_bias = std::max(exp_bias, int32_t(-32));
  }
""" % mark
s = s.replace(anchor, add + anchor)
open(path, "w", encoding="utf-8", newline="\n").write(s)
print("fixed:", path)
