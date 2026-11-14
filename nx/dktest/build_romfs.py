# Fills nx/dktest/romfs for the deko3d test NRO (run in WSL):
#   python3 build_romfs.py [dksh_package=~/sd-shaders/dksh]
# - dktest_vs.dksh / dktest_ps.dksh: the test shaders, through the same HLSL -> SPIR-V -> GLSL -> uam path as the
#   library (nx/shaders/tools/dk_build.py, compile_one without SDNX_DEKO3D: they are written for deko3d);
# - dksh/: the deko3d shader package as dk_build.py made it (index.txt, textures.txt, *.dksh);
# - dksh_test.txt: what the test needs to draw each one that the renderer gets elsewhere (from the Xenos vertex
#   fetches): "<name> <storage buffer mask> <vertex input location mask>" (hex), read from the package's GLSL.
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'shaders', 'tools'))
import dk_build  # noqa: E402

package = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/sd-shaders/dksh')
romfs = os.path.join(HERE, 'romfs')
work = os.path.join(HERE, 'shaderbuild')
shutil.rmtree(romfs, ignore_errors=True)
os.makedirs(romfs)

for name, stage in (('dktest_vs', 'vert'), ('dktest_ps', 'frag')):
    b, status, why, _ = dk_build.compile_one(os.path.join(HERE, 'shaders', name + '.hlsl'), work, stage, defines=())
    if status != 'ok':
        sys.exit('%s: %s: %s (see %s)' % (name, status, why, os.path.join(work, 'err', name + '.txt')))
    shutil.copy(os.path.join(work, name + '.dksh'), romfs)
    print(name, 'ok')

shutil.copytree(package, os.path.join(romfs, 'dksh'))
SSBO = re.compile(r'layout\(binding = (\d+), std430\) (?:readonly )?buffer ')
INPUT = re.compile(r'^layout\(location = (\d+)\) in ', re.M)
lines = []
for line in open(os.path.join(package, 'index.txt')):
    kind, key, name = line.split()[:3]
    glsl = open(os.path.join(package + '.work', 'glsl', name + '.glsl')).read()
    ssbos = sum(1 << int(n) for n in set(SSBO.findall(glsl)))
    inputs = sum(1 << int(n) for n in set(INPUT.findall(glsl))) if kind == 'vs' else 0
    lines.append('%s %x %x' % (name, ssbos, inputs))
open(os.path.join(romfs, 'dksh_test.txt'), 'w').write('\n'.join(lines) + '\n')
print('package', len(lines), 'shaders')
