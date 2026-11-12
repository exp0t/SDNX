#!/usr/bin/env bash
# Sleeping Dogs -> Switch: build the NRO in the devkitpro/devkita64 container (run inside WSL Ubuntu, after
# nx/tools/sd_codegen.sh).
#
#   SUDO_PW=... bash /mnt/d/CODE/SDNX/nx/tools/sd_build_nro.sh        (docker needs sudo here; or be in group docker)
#
# Output: ~/masseffect-nx/out/nx-sd/sdretail.nro, copied to D:\CODE\SDNX\nx\sdretail.nro
# Environment: JOBS (default 4: each job needs 1-2 GB, WSL has ~7 GB), NRO_OUT (default out/nx-sd)
set -euo pipefail

ME="$HOME/masseffect-nx"
MESA=/mnt/d/CODE/SDNX/mesa-sdk
NRO_OUT="${NRO_OUT:-out/nx-sd}"
JOBS="${JOBS:-4}"
DEST=/mnt/d/CODE/SDNX/nx

docker_cmd() {
  if docker info >/dev/null 2>&1; then docker "$@";
  elif [[ -n "${SUDO_PW:-}" ]]; then echo "$SUDO_PW" | sudo -S -p "" docker "$@";
  else sudo docker "$@"; fi
}

[[ -f "$ME/sdretail/generated/rexglue.cmake" ]] || { echo "error: run nx/tools/sd_codegen.sh first" >&2; exit 1; }
[[ -f "$MESA/opt/devkitpro/portlibs/switch/lib/libvulkan.a" ]] || { echo "error: no NVK SDK in $MESA" >&2; exit 1; }

echo "== build $NRO_OUT (-j$JOBS)"
docker_cmd run --rm -v "$ME:$ME" -v "$MESA:$MESA:ro" -w "$ME" \
  -e ME="$ME" -e MESA="$MESA" -e NRO_OUT="$NRO_OUT" -e JOBS="$JOBS" \
  devkitpro/devkita64:latest bash -euo pipefail -c '
    cmake -S sdretail -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ME/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA/opt/devkitpro/portlibs/switch" > "$NRO_OUT.configure.log" 2>&1 \
      || { tail -40 "$NRO_OUT.configure.log"; exit 1; }
    cmake --build "$NRO_OUT" -j "$JOBS"
    # crt0 must be first in .text or the NRO does not boot (masseffect-nx tools/build_nro.sh)
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/sdretail" | grep "^0000000000000000 T _start$" > /dev/null
    echo "OK: _start at 0"
  '
cp -f "$ME/$NRO_OUT/sdretail.nro" "$DEST/sdretail.nro"
ls -la "$DEST/sdretail.nro"
