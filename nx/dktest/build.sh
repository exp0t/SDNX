#!/bin/bash
# Builds the deko3d test NRO: romfs (test shaders + the deko3d shader package from nx/shaders/tools/dk_build.py), then the NRO in the
# devkitA64 image. Copies it to sd_card/sdnx_dktest. Run in WSL:  SUDO_PW=... bash nx/dktest/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SDNX=$(cd "$HERE/../.." && pwd)
python3 "$HERE/build_romfs.py" "${DKSH_PKG:-$HOME/sd-shaders/dksh}"
docker_cmd() {
  if docker info >/dev/null 2>&1; then docker "$@";
  elif [[ -n "${SUDO_PW:-}" ]]; then echo "$SUDO_PW" | sudo -S -p "" docker "$@";
  else sudo docker "$@"; fi
}
docker_cmd run --rm -v "$SDNX:$SDNX" -w "$HERE" -u "$(id -u):$(id -g)" devkitpro/devkita64:latest \
  bash -c 'make -j"$(nproc)"'
mkdir -p "$SDNX/sd_card/sdnx_dktest"
cp "$HERE/sdnx_dktest.nro" "$SDNX/sd_card/sdnx_dktest/"
ls -la "$SDNX/sd_card/sdnx_dktest"
