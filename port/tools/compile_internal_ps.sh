#!/usr/bin/env bash
# Compile the hand-built pixel-shader container's HLSL with the library flags, then validate.
set -u
export DXC="$HOME/tools/dxc/bin/dxc"
export VAL="$HOME/tools/spvtools/tools/spirv-val"
export LD_LIBRARY_PATH="$HOME/tools/dxc/lib"
cd "$HOME/sd-intps/out" || exit 1
rm -f ps_internal.spv ps_internal.err
"$DXC" -spirv -T ps_6_6 -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
  -Werror=parameter-usage -Fo ps_internal.spv ps_internal.hlsl 2>ps_internal.err
rc=$?
echo "dxc rc=$rc"
[ -s ps_internal.err ] && head -5 ps_internal.err
ls -l ps_internal.spv 2>/dev/null || echo NO_SPV
"$VAL" --target-env vulkan1.2 --scalar-block-layout ps_internal.spv && echo VALID
