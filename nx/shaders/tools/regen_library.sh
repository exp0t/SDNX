#!/usr/bin/env bash
# Rebuilds the translator from nx/shaders and translates every container again, then compiles the Vulkan SPIR-V:
#   bash nx/shaders/tools/regen_library.sh <out_dir>      -> <out_dir>/{hlsl,spv}
# Used to check that a change of the translator or shader_common.h leaves the Vulkan library bit-identical
# (cmp the .spv against a run made before the change). Containers: ~/sd-shaders/containers (build_shaderlib.sh).
set -euo pipefail
OUT=$1
R=/mnt/d/CODE/SDNX/nx
W=$HOME/sd-shaders
XXHASH_DIR=$HOME/masseffect-nx/sdk/thirdparty/xxHash FMT_DIR=$HOME/masseffect-nx/sdk/thirdparty/fmt/include \
  bash $R/shaders/tools/build_xenos_hlsl.sh >/dev/null
TOOL=$OUT/xenos_hlsl
mkdir -p "$OUT/hlsl" "$OUT/spv"
cp $R/out/tools/xenos_hlsl "$TOOL"
translate() {
  f=$1; b=$(basename "$f" .bin); t=$(mktemp -d); ln -s "$f" "$t/$b.bin"
  "$TOOL" "$t" "$t/o" "$R/shaders/XenosRecomp/shader_common.h" >/dev/null 2>&1 && mv "$t/o/$b.hlsl" "$OUT/hlsl/"
  rm -rf "$t"
}
export -f translate; export TOOL OUT R
ls $W/containers/*.bin | xargs -P 16 -I{} bash -c 'translate {}'
echo "hlsl: $(ls $OUT/hlsl | wc -l) of $(ls $W/containers | wc -l)"
export DXC=$HOME/tools/dxc/bin/dxc VAL=$HOME/tools/spvtools/tools/spirv-val LD_LIBRARY_PATH=$HOME/tools/dxc/lib
OUT=$OUT/spv; export OUT
ls "$OUT/../hlsl"/*.hlsl | xargs -P 16 -n 1 sh $R/shaders/tools/compile_spirv_one.sh
echo "spv: $(ls $OUT/*.spv | wc -l), errors: $(ls $OUT/*.err 2>/dev/null | wc -l)"
