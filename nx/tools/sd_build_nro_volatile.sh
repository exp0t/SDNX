#!/usr/bin/env bash
# A/B build of the NRO with guest loads/stores volatile again (REX_GUEST_VOLATILE=volatile; the default Switch build
# strips them with pch_no_volatile.py, the desktop bench keeps them). Separate build tree: out/nx-sd-vol.
#   SUDO_PW=... JOBS=4 bash /mnt/d/CODE/SDNX/nx/tools/sd_build_nro_volatile.sh
# Output: D:\CODE\SDNX\nx\sdretail_volatile.nro
set -euo pipefail
ME="$HOME/masseffect-nx"
MESA=/mnt/d/CODE/SDNX/mesa-sdk
NRO_OUT=out/nx-sd-vol
JOBS="${JOBS:-4}"
docker_cmd() {
  if docker info >/dev/null 2>&1; then docker "$@";
  elif [[ -n "${SUDO_PW:-}" ]]; then echo "$SUDO_PW" | sudo -S -p "" docker "$@";
  else sudo docker "$@"; fi
}
docker_cmd run --rm -v "$ME:$ME" -v "$MESA:$MESA:ro" -w "$ME" \
  -e ME="$ME" -e MESA="$MESA" -e NRO_OUT="$NRO_OUT" -e JOBS="$JOBS" \
  devkitpro/devkita64:latest bash -euo pipefail -c '
    cmake -S sdretail -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ME/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro \
      -DSDRETAIL_GUEST_VOLATILE=ON \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA/opt/devkitpro/portlibs/switch" > "$NRO_OUT.configure.log" 2>&1 \
      || { tail -40 "$NRO_OUT.configure.log"; exit 1; }
    cmake --build "$NRO_OUT" -j "$JOBS"
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/sdretail" | grep "^0000000000000000 T _start$" > /dev/null
    echo "OK: _start at 0"
  '
cp -f "$ME/$NRO_OUT/sdretail.nro" /mnt/d/CODE/SDNX/nx/sdretail_volatile.nro
ls -la /mnt/d/CODE/SDNX/nx/sdretail_volatile.nro
