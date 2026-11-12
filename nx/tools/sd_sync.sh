#!/usr/bin/env bash
# Copy the NRO sources from D:\CODE\SDNX\nx\sdretail to ~/masseffect-nx/sdretail without touching generated/
# (no codegen, no post-codegen patches): for changes in src/ or the toml/CMake files only. Unchanged files keep
# their timestamps, so the next sd_build_nro.sh rebuilds only what changed.
set -euo pipefail
APP="$HOME/masseffect-nx/sdretail"
SRC=/mnt/d/CODE/SDNX/nx/sdretail
mkdir -p "$APP/src"
copy() {  # src dst
  sed 's/\r$//' "$1" > "$2.tmp"
  if cmp -s "$2.tmp" "$2"; then rm "$2.tmp"; else mv "$2.tmp" "$2"; echo "updated $2"; fi
}
for f in CMakeLists.txt sdretail_manifest.toml overrides.toml perf_overrides.toml sdretail.toml; do
  copy "$SRC/$f" "$APP/$f"
done
for f in "$SRC"/src/*; do copy "$f" "$APP/src/$(basename "$f")"; done
