#!/usr/bin/env bash
# Sleeping Dogs shader library, offline (WSL). Steps:
#   1. containers:  py port/tools/sdshaders.py <game_root> port/shaders/containers     (Windows side)
#   2. HLSL:        XenosRecomp from masseffect-nx with the SDNX run-time vertex fetch swizzle (nx/shaders, built
#                   by nx/shaders/tools/build_xenos_hlsl.sh -> ~/tools/xenos_hlsl_sdnx)
#   3. SPIR-V:      DXC 1.9 (~/tools/dxc) + spirv-val (built from sdk/thirdparty/spirv-tools -> ~/tools/spvtools)
#   4. hashes:      XXH3-64 of each container's microcode, as the SDK hashes guest memory (ucode_hash.c)
# Results in ~/sd-shaders/{containers,hlsl,spv}, port/shaders/ucode_hashes.txt. Game data never leaves the PC.
set -e
SRC=/mnt/d/CODE/SDNX/port/shaders/containers
W=$HOME/sd-shaders
mkdir -p $W && rm -rf $W/containers && cp -r $SRC $W/containers && rm -f $W/containers/containers.tsv
R=/mnt/d/CODE/SDNX/nx
XXHASH_DIR=$HOME/masseffect-nx/sdk/thirdparty/xxHash FMT_DIR=$HOME/masseffect-nx/sdk/thirdparty/fmt/include bash $R/shaders/tools/build_xenos_hlsl.sh
cp $R/out/tools/xenos_hlsl $HOME/tools/xenos_hlsl_sdnx
rm -rf $W/hlsl $W/spv; mkdir -p $W/hlsl
for f in $W/containers/*.bin; do b=$(basename $f .bin); t=$(mktemp -d); ln -s $f $t/$b.bin
  $HOME/tools/xenos_hlsl_sdnx $t $t/o $R/shaders/XenosRecomp/shader_common.h >/dev/null 2>&1 && mv $t/o/$b.hlsl $W/hlsl/; rm -rf $t; done
echo "hlsl: $(ls $W/hlsl | wc -l) of $(ls $W/containers | wc -l)"
export DXC=$HOME/tools/dxc/bin/dxc VAL=$HOME/tools/spvtools/tools/spirv-val OUT=$W/spv LD_LIBRARY_PATH=$HOME/tools/dxc/lib
mkdir -p $OUT
ls $W/hlsl/*.hlsl | xargs -P 12 -n 1 sh $HOME/me-shaders/shaders/tools/compile_spirv_one.sh
echo "spv: $(ls $OUT/*.spv | wc -l), errors: $(ls $OUT/*.err 2>/dev/null | wc -l)"
gcc -O2 -I$HOME/masseffect-nx/sdk/thirdparty/xxHash -o $HOME/tools/uhash /mnt/d/CODE/SDNX/port/tools/shaderlib/ucode_hash.c
(cd $W/containers && $HOME/tools/uhash *.bin | sed 's/\.bin$//') > /mnt/d/CODE/SDNX/port/shaders/ucode_hashes.txt

# 5. Run-time package for the native renderer: <out>/index.txt + <out>/<name>.spv
#    index line: "<ps|vs> <key hex> <name> [K:addr:reg:mask ...]"; ps key = raw microcode XXH3, vs key = XXH3 with the
#    vertex fetches zeroed (one entry serves every Direct3D variant); the fetch order comes from the HLSL.
PKG=${PKG:-/mnt/d/CODE/SDNX/nx/desktop/out/sdnx_shaders}
rm -rf "$PKG"; mkdir -p "$PKG"
(cd $W/containers && $HOME/tools/uhash *.bin) > $W/keys.txt
python3 - "$W" "$PKG" <<'PY'
import os, sys, shutil
w, pkg = sys.argv[1], sys.argv[2]
seen = set(); lines = []; dup = 0; skipped_computed = []
for line in open(os.path.join(w, 'keys.txt')):
    raw, masked, fn = line.split()
    name = fn[:-4]; kind = name[:2]
    spv = os.path.join(w, 'spv', name + '.spv')
    if not os.path.exists(spv): continue
    key = raw if kind == 'ps' else masked
    if (kind, key) in seen: dup += 1; continue
    seen.add((kind, key))
    order = ''
    computed = False
    if kind == 'vs':
        for l in open(os.path.join(w, 'hlsl', name + '.hlsl')):
            if l.startswith('// SDNX_FETCH_ORDER:'): order = l.split(':', 1)[1].strip()
            # Hand-made instancing (vertex index read from a stream): the shader pulls its vertices itself.
            if l.startswith('// SDNX_COMPUTED_INDEX_FETCH'): computed = True
    if computed: skipped_computed.append(name); order = (order + ' pull').strip()
    shutil.copy(spv, os.path.join(pkg, name + '.spv'))
    lines.append(('%s %s %s %s' % (kind, key, name, order)).rstrip())
open(os.path.join(pkg, 'index.txt'), 'w').write('\n'.join(lines) + '\n')
print('package: %d entries (%d duplicate keys skipped) -> %s' % (len(lines), dup, pkg))
print('pulling their vertices (computed-index instancing): %s' % ' '.join(skipped_computed))
PY

# 6. Run-time Direct3D shaders that are not in the game containers (clears through rectangle lists):
#    port/shaders/internal/index.txt lists them; a .hlsl is compiled (compile_internal_hlsl.sh), otherwise the
#    prebuilt .spv is copied.
INT=/mnt/d/CODE/SDNX/port/shaders/internal
while read -r kind key name rest; do
  [ -n "$name" ] || continue
  if [ -f "$INT/$name.hlsl" ]; then
    PKG_OUT="$PKG" bash /mnt/d/CODE/SDNX/port/tools/compile_internal_hlsl.sh "$name" "$kind" >/dev/null
  else
    cp "$INT/$name.spv" "$PKG/$name.spv"
  fi
  echo "$kind $key $name $rest" | sed 's/ *$//' >> "$PKG/index.txt"
done < <(tr -d '\r' < "$INT/index.txt")
echo "internal shaders added from $INT"
