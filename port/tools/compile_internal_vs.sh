#!/usr/bin/env bash
# Compile the hand-built container's HLSL with the same flags as the shader library, then validate.
set -u
export DXC="$HOME/tools/dxc/bin/dxc"
export VAL="$HOME/tools/spvtools/tools/spirv-val"
export LD_LIBRARY_PATH="$HOME/tools/dxc/lib"
cd "$HOME/sd-int/out" || exit 1
rm -f vs_internal.spv vs_internal.err
"$DXC" -spirv -T vs_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
  -Werror=parameter-usage -fvk-invert-y -Fo vs_internal.spv vs_internal.hlsl 2>vs_internal.err
rc=$?
echo "dxc rc=$rc"
[ -s vs_internal.err ] && head -5 vs_internal.err
ls -l vs_internal.spv 2>/dev/null || echo NO_SPV
"$VAL" --target-env vulkan1.2 --scalar-block-layout vs_internal.spv && echo VALID
