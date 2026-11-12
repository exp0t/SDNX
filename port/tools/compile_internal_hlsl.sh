#!/usr/bin/env bash
# Compiles a hand-written internal-shader HLSL (port/shaders/internal/<name>.hlsl) with the shader library flags,
# validates it and copies the SPIR-V into the desktop package. Usage: compile_internal_hlsl.sh <name> <vs|ps>
set -eu
name=$1; kind=$2
src=/mnt/d/CODE/SDNX/port/shaders/internal
out=${PKG_OUT:-/mnt/d/CODE/SDNX/nx/desktop/out/sdnx_shaders}
export LD_LIBRARY_PATH="$HOME/tools/dxc/lib"
profile=vs_6_6; extra=-fvk-invert-y
[ "$kind" = ps ] && { profile=ps_6_6; extra=; }
"$HOME/tools/dxc/bin/dxc" -spirv -T $profile -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
  -Werror=parameter-usage $extra -Fo "$src/$name.spv" "$src/$name.hlsl"
"$HOME/tools/spvtools/tools/spirv-val" --target-env vulkan1.2 --scalar-block-layout "$src/$name.spv"
cp "$src/$name.spv" "$out/$name.spv"
echo "ok: $out/$name.spv"
