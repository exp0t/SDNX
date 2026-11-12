#!/usr/bin/env python3
"""Fix a merge bug in the masseffect-nx SDK SPIR-V translator (sdk/src/graphics/pipeline/shader/spirv_translator.cpp).

In v0.2.1 the declarations of two function-local variables are interleaved:

    var_main_tfetch_lod_ =
    var_main_vfetch_bound_ =
        createVariable(int,   "xe_var_vfetch_bound");
        createVariable(float, "xe_var_tfetch_lod");      <- result discarded

so the texture-fetch LOD and the vertex-fetch bound are the same int variable and float LODs are stored into it:
invalid SPIR-V (spirv-val: "OpStore ... type does not match"). The NVIDIA desktop compiler crashes on it; NVK on the
Switch accepts it, but texture LOD computations are then wrong. Idempotent.

Usage: fix_tfetch_lod.py <sdk root>
"""
import sys

path = sys.argv[1].rstrip("/\\") + "/src/graphics/pipeline/shader/spirv_translator.cpp"
s = open(path, encoding="utf-8").read()
bad = """    var_main_tfetch_lod_ =
    var_main_vfetch_bound_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_int_,
                                 "xe_var_vfetch_bound", const_int_0_);
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float_,
                                 "xe_var_tfetch_lod", const_float_0_);
"""
good = """    var_main_vfetch_bound_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_int_,
                                 "xe_var_vfetch_bound", const_int_0_);
    var_main_tfetch_lod_ =
        builder_->createVariable(spv::NoPrecision, spv::StorageClassFunction, type_float_,
                                 "xe_var_tfetch_lod", const_float_0_);
"""
s2 = s.replace("\r\n", "\n")
if good in s2:
    print("already fixed:", path)
elif bad in s2:
    open(path, "w", encoding="utf-8", newline="\n").write(s2.replace(bad, good))
    print("fixed:", path)
else:
    sys.exit("pattern not found: " + path)
