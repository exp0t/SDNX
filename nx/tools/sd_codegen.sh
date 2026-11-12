#!/usr/bin/env bash
# Sleeping Dogs -> Switch: code generation + post-codegen steps (run inside WSL Ubuntu).
#
#   bash /mnt/d/CODE/SDNX/nx/tools/sd_codegen.sh
#   SKIP_CODEGEN=1 bash ...   only sync + post-codegen patches (after changing hooks in src/)
#
# 1. copies the project sources from D:\CODE\SDNX\nx\sdretail (the canonical copy) to ~/masseffect-nx/sdretail
#    (the SDK tree it builds against), keeping generated/;
# 2. runs the host code generator (~/masseffect-nx/sdk/out/host/rexglue);
# 3. post-codegen patches from masseffect-nx/tools (generic, only their masseffect_ file prefix is replaced):
#      direct_calls.py     direct calls to functions our sources do not hook (REX_HOOK_RAW(sub_X) in src/)
#      pch_no_volatile.py  guest loads/stores without volatile
# 4. register-use checks (read_before_write.py) for non_volatile_as_local / non_argument_as_local: every
#    function listed there needs share_registers in perf_overrides.toml, or a review.
set -euo pipefail

ME="$HOME/masseffect-nx"
SRC=/mnt/d/CODE/SDNX/nx/sdretail
APP="$ME/sdretail"
GEN="$APP/generated/default"
TOOLS_SD="$APP/.tools_sd"
REXGLUE="${REXGLUE:-$ME/sdk/out/host/rexglue}"

echo "== sync sources $SRC -> $APP"
mkdir -p "$APP/src"
for f in CMakeLists.txt sdretail_manifest.toml overrides.toml perf_overrides.toml sdretail.toml; do
  sed 's/\r$//' "$SRC/$f" > "$APP/$f"
done
for f in "$SRC"/src/*; do sed 's/\r$//' "$f" > "$APP/src/$(basename "$f")"; done

if [[ "${SKIP_CODEGEN:-0}" != 1 ]]; then
  echo "== codegen"
  ( cd "$APP" && "$REXGLUE" codegen sdretail_manifest.toml ) 2>&1 | tee "$APP/codegen.log" | tail -5
fi

echo "== post-codegen patches"
mkdir -p "$TOOLS_SD"
for t in direct_calls.py pch_no_volatile.py pch_common.py read_before_write.py; do
  sed 's/masseffect_/sdretail_/g' "$ME/tools/$t" > "$TOOLS_SD/$t"
done
# Functions our sources hook must keep being called through sub_X (the hook), not __imp__sub_X.
cat "$APP"/src/*.cpp "$APP"/src/*.h 2>/dev/null | grep -o 'REX_HOOK_RAW(sub_[0-9A-F]*' | grep -o 'sub_[0-9A-F]*' \
  | sort -u > "$TOOLS_SD/hooked.txt" || true
echo "hooked: $(paste -sd' ' "$TOOLS_SD/hooked.txt")"
python3 "$TOOLS_SD/direct_calls.py" --gen "$GEN" --undo > /dev/null
python3 "$TOOLS_SD/direct_calls.py" --gen "$GEN" --hooked-list "$TOOLS_SD/hooked.txt"
python3 "$TOOLS_SD/pch_no_volatile.py" "$GEN/sdretail_pch.h"

echo "== register-use checks (read_before_write.py)"
# Expected (2026-11-08, see perf_overrides.toml): non-volatile = 5 harmless functions; --volatile = none.
python3 "$TOOLS_SD/read_before_write.py" "$GEN" > "$APP/read_before_write.txt" 2>&1
python3 "$TOOLS_SD/read_before_write.py" "$GEN" --volatile > "$APP/read_before_write_volatile.txt" 2>&1
echo "-- non-volatile registers:"; grep -v '^#' "$APP/read_before_write.txt" || true
echo "-- argument registers (--volatile):"; grep -v '^#' "$APP/read_before_write_volatile.txt" || true
echo "lists: $APP/read_before_write*.txt"
