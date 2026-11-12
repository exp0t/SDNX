#!/usr/bin/env python3
"""ReXGlue / masseffect-nx SDK (and upstream Xenia): texture fetch ignores the integer number format.

Xenos texture fetch constants have num_format (word 3 bit 0): 0 = fraction (normalized), 1 = integer. With integer
the fetch returns the raw stored integer (e.g. 0..65535 for 16 bits) before exp_adjust. Xenia always samples fixed
formats as normalized, so an integer fetch comes back 65535 (or 255) times too small.

Sleeping Dogs samples its light buffer (k_16_16_16_16, resolved as unsigned integer with exp_bias +11) with
num_format = int and exp_adjust = -11: integer * 2^-11 = the original HDR value. In Xenia the scene composite got
value / 2^16 - no sunlight, no sun shafts, flat ambient-only picture. Found 2026-11-08 (frame_frame6851, fetch
constant slot 4: fmt 26, base 0F867000, num_format int, exp_adjust -11). Works together with
fix_resolve_uint.py (the resolve side).

Fix: in the tfetch result exponent bias, multiply by 65535 for k_16 / k_16_16 / k_16_16_16_16 and by 255 for
k_8 / k_8_8 / k_8_8_8_8 when num_format is integer. Idempotent.

Usage: fix_tfetch_numformat.py <sdk root>
"""
import sys

path = sys.argv[1].rstrip("/\\") + "/src/graphics/pipeline/shader/spirv_translator_fetch.cpp"
s = open(path, encoding="utf-8").read().replace("\r\n", "\n")
mark = "SDNX: integer num_format"
if mark in s:
    print("already fixed:", path)
    sys.exit(0)
old = """        spv::Id result_exponent_bias = builder_->createBinBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450Ldexp, const_float_1_,
            builder_->createTriOp(spv::OpBitFieldSExtract, type_int_, fetch_constant_word_3_signed,
                                  builder_->makeUintConstant(13), builder_->makeUintConstant(6)));
"""
if s.count(old) != 1:
    sys.exit("pattern not found: " + path)
new = """        // %s: word 3 bit 0 = 1 - the fetch returns the raw integer, while the
        // host image is sampled as normalized - scale by the maximum value of the format (word 1 bits 0:5).
        spv::Id result_number_scale;
        {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(const_int_0_);
          id_vector_temp_.push_back(
              builder_->makeIntConstant(int((fetch_constant_word_0_index + 1) >> 2)));
          id_vector_temp_.push_back(
              builder_->makeIntConstant(int((fetch_constant_word_0_index + 1) & 3)));
          spv::Id fetch_constant_word_1_format = builder_->createBinOp(
              spv::OpBitwiseAnd, type_uint_,
              builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                               uniform_fetch_constants_,
                                                               id_vector_temp_),
                                   spv::NoPrecision),
              builder_->makeUintConstant(63));
          spv::Id is_integer = builder_->createBinOp(
              spv::OpINotEqual, type_bool_,
              builder_->createBinOp(
                  spv::OpBitwiseAnd, type_uint_,
                  builder_->createUnaryOp(spv::OpBitcast, type_uint_, fetch_constant_word_3_signed),
                  builder_->makeUintConstant(1)),
              builder_->makeUintConstant(0));
          auto format_is = [&](uint32_t format) {
            return builder_->createBinOp(spv::OpIEqual, type_bool_, fetch_constant_word_1_format,
                                         builder_->makeUintConstant(format));
          };
          spv::Id is_16bpc = builder_->createBinOp(
              spv::OpLogicalOr, type_bool_,
              builder_->createBinOp(spv::OpLogicalOr, type_bool_,
                                    format_is(uint32_t(xenos::TextureFormat::k_16)),
                                    format_is(uint32_t(xenos::TextureFormat::k_16_16))),
              format_is(uint32_t(xenos::TextureFormat::k_16_16_16_16)));
          spv::Id is_8bpc = builder_->createBinOp(
              spv::OpLogicalOr, type_bool_,
              builder_->createBinOp(spv::OpLogicalOr, type_bool_,
                                    format_is(uint32_t(xenos::TextureFormat::k_8)),
                                    format_is(uint32_t(xenos::TextureFormat::k_8_8))),
              format_is(uint32_t(xenos::TextureFormat::k_8_8_8_8)));
          spv::Id format_max = builder_->createTriOp(
              spv::OpSelect, type_float_, is_16bpc, builder_->makeFloatConstant(65535.0f),
              builder_->createTriOp(spv::OpSelect, type_float_, is_8bpc,
                                    builder_->makeFloatConstant(255.0f), const_float_1_));
          result_number_scale =
              builder_->createTriOp(spv::OpSelect, type_float_, is_integer, format_max, const_float_1_);
        }
        spv::Id result_exponent_bias = builder_->createBinBuiltinCall(
            type_float_, ext_inst_glsl_std_450_, GLSLstd450Ldexp, result_number_scale,
            builder_->createTriOp(spv::OpBitFieldSExtract, type_int_, fetch_constant_word_3_signed,
                                  builder_->makeUintConstant(13), builder_->makeUintConstant(6)));
""" % mark
s = s.replace(old, new)
open(path, "w", encoding="utf-8", newline="\n").write(s)
print("fixed:", path)
