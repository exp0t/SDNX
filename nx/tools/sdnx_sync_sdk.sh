#!/usr/bin/env bash
# Sleeping Dogs -> Switch: copy the native renderer (sdnx backend) from the desktop SDK fork (D:\CODE\me-sdk, where it
# is developed and tested on the bench) into the SDK the NRO builds against (~/masseffect-nx/sdk). Run inside WSL,
# then nx/tools/sd_build_nro.sh (the NRO build compiles the SDK from source: REXSDK_DIR).
#
# Copied: src/graphics/sdnx/, include/rex/graphics/sdnx/, and the three files that hook it in:
#   src/graphics/CMakeLists.txt (the sdnx sources), src/graphics/plugin_main.cpp (backend choice: cvar
#   sdnx_native_renderer), include/rex/ui/vulkan/functions/device_1_0.inc (vkCmdBlitImage, vkCmdCopyImage, ...).
# The other differences between the two SDKs are emulation/debug changes of the desktop bench and stay out.
# Unchanged files keep their timestamps (no needless rebuild).
set -euo pipefail
SRC=/mnt/d/CODE/me-sdk
DST="$HOME/masseffect-nx/sdk"
copy() {  # relative path
  mkdir -p "$(dirname "$DST/$1")"
  sed 's/\r$//' "$SRC/$1" > "$DST/$1.tmp"
  if cmp -s "$DST/$1.tmp" "$DST/$1"; then rm "$DST/$1.tmp"; else mv "$DST/$1.tmp" "$DST/$1"; echo "updated $1"; fi
}
for d in src/graphics/sdnx include/rex/graphics/sdnx; do
  (cd "$SRC" && find "$d" -type f) | while read -r f; do copy "$f"; done
done
copy src/graphics/CMakeLists.txt
copy src/graphics/plugin_main.cpp
copy include/rex/ui/vulkan/functions/device_1_0.inc
