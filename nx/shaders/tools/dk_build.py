# deko3d shader package: the HLSL of the shader library -> DKSH, for the deko3d backend of the native renderer.
#   python3 dk_build.py <vulkan package dir> <hlsl dir> <out package dir> [jobs]
# For every entry of the Vulkan package's index.txt (the game shaders, deduplicated, and the internal ones) the same
# HLSL is compiled with -DSDNX_DEKO3D (shader_common.h): DXC -> SPIR-V -> SPIRV-Cross (GLSL 4.60, no Vulkan
# semantics) -> uam -> DKSH. Output:
#   <out>/index.txt        the Vulkan index as it is (kind, key, name, vertex fetch order): the same lookup
#   <out>/<name>.dksh
#   <out>/textures.txt     one line per shader: "<name> <binding>:<2d|3d|cube>:<slot> ..." - the texture bindings,
#                          given here in order (deko3d has 32 per stage), and the Xenos fetch slot each one reads
# The internal shaders the Vulkan package has only as SPIR-V come from port/shaders/internal/<name>.xenos.hlsl.
# HLSL files that embed an older copy of shader_common.h (hand-written internal shaders, port/shaders/internal)
# get the current one: the embedded block is replaced and vk::RawBufferLoad becomes SDNX_RAW_LOAD.
# Also importable: compile_one(src, work, stage) for shaders written for deko3d already (nx/dktest).
# Needs DXC, SPIRV_CROSS, UAM (defaults under ~/tools).
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.join(HERE, '..', 'XenosRecomp', 'shader_common.h')
DXC = os.environ.get('DXC', os.path.expanduser('~/tools/dxc/bin/dxc'))
CROSS = os.environ.get('SPIRV_CROSS', os.path.expanduser('~/tools/spirv-cross-build/spirv-cross'))
UAM = os.environ.get('UAM', os.path.expanduser('~/tools/uam'))
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser('~/tools/dxc/lib'))

FORBIDDEN = ('GL_EXT_buffer_reference', 'GL_ARB_gpu_shader_int64', 'GL_EXT_shader_explicit_arithmetic_types',
             'GL_EXT_nonuniform_qualifier', 'GL_EXT_samplerless_texture_functions')
SAMPLER_RE = re.compile(r'^(?:layout\(binding = \d+\) )?uniform (sampler2D|sampler3D|samplerCube) (\w+);\n', re.M)
NAME_RE = re.compile(r'SPIRV_Cross_Combinedg_dkT(2D|3D|Cube)_(\d+)(?:g_dkS_(\d+)|SPIRV_Cross_DummySampler)$')
# A dynamic component index into a UBO vector: uam reads it as .x every time (rexglue-sdk-nx, glsl.cpp).
DYNAMIC_COMPONENT_RE = re.compile(r'g_Ubo\w*\.v\[[^\]]*\]\[[^\]0-9u][^\]]*\]')


def refresh_common(text):
    """An HLSL that embeds shader_common.h (the translator's output, or an internal shader made from it) with the
    current one. Unchanged if it has none."""
    start = text.find('#ifndef SHADER_COMMON_H_INCLUDED')
    if start < 0:
        return text
    depth, i = 0, start
    for m in re.finditer(r'^\s*#\s*(if|ifdef|ifndef|endif)\b.*$', text[start:], re.M):
        depth += -1 if m.group(1) == 'endif' else 1
        if depth == 0:
            i = start + m.end()
            break
    else:
        raise ValueError('unterminated SHADER_COMMON_H_INCLUDED block')
    # The two lines the translator puts before the guard (the SDNX comment and the swizzle define) stay.
    common = open(COMMON, encoding='utf-8').read()
    cstart = common.find('#ifndef SHADER_COMMON_H_INCLUDED')
    body = text[i:].replace('vk::RawBufferLoad<', 'SDNX_RAW_LOAD<')
    return text[:start] + common[cstart:].rstrip('\n') + body


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
    return r.returncode, r.stdout + r.stderr


def bind_textures(glsl):
    """Gives the combined samplers bindings 0..n-1 (by slot, then type) and returns (glsl, [(binding, type, slot)])."""
    found = {}
    for kind, name in SAMPLER_RE.findall(glsl):
        m = NAME_RE.match(name)
        if not m:
            raise ValueError('unexpected sampler ' + name)
        dim, slot = m.group(1).lower(), int(m.group(2))
        if m.group(3) is not None and int(m.group(3)) != slot:
            raise ValueError('texture and sampler of different slots: ' + name)
        found[name] = (dim, slot, kind)
    # A size query of a texture that is also sampled gets a dummy sampler: textureSize works on the real one.
    for name, (dim, slot, kind) in list(found.items()):
        if name.endswith('SPIRV_Cross_DummySampler'):
            real = [n for n, v in found.items() if v[:2] == (dim, slot) and not n.endswith('DummySampler')]
            if real:
                glsl = re.sub(r'^(?:layout\(binding = \d+\) )?uniform \w+ %s;\n' % name, '', glsl, flags=re.M)
                glsl = glsl.replace(name, real[0])
                del found[name]
    order = sorted(found.items(), key=lambda kv: (kv[1][1], kv[1][0]))
    if len(order) > 32:
        raise ValueError('%d textures, deko3d has 32 per stage' % len(order))
    table = []
    for binding, (name, (dim, slot, kind)) in enumerate(order):
        glsl = re.sub(r'^(?:layout\(binding = \d+\) )?uniform %s %s;' % (kind, name),
                      'layout(binding = %d) uniform %s %s;' % (binding, kind, name), glsl, flags=re.M)
        table.append((binding, dim, slot))
    return glsl, table


def compile_one(src, work, stage=None, defines=('SDNX_DEKO3D',), name=None):
    """One HLSL file to <work>/<name>.dksh. Returns (name, 'ok' or the failed step, message, texture table)."""
    name = name or os.path.basename(src)[:-5]
    stage = stage or ('frag' if name.startswith('ps_') else 'vert')
    for d in ('hlsl', 'spv', 'glsl', 'err'):
        os.makedirs(os.path.join(work, d), exist_ok=True)
    err = os.path.join(work, 'err', name + '.txt')

    def fail(step, message):
        open(err, 'w').write(message)
        lines = [l for l in message.strip().splitlines() if 'error' in l.lower()] or message.strip().splitlines()
        return name, step, (lines[0] if lines else '')[:200], None
    try:
        text = refresh_common(open(src, encoding='utf-8', errors='replace').read())
    except Exception as e:
        return fail('common', str(e))
    h = os.path.join(work, 'hlsl', name + '.hlsl')
    open(h, 'w').write(text)
    spv = os.path.join(work, 'spv', name + '.spv')
    profile = 'ps_6_6' if stage == 'frag' else 'vs_6_6'
    # No -fvk-invert-y: deko3d's clip-space y points up, as Direct3D's (nx/deko3d/.../nucleo.h).
    rc, log = run([DXC, '-spirv', '-T', profile, '-E', 'main', '-HV', '2021', '-fspv-target-env=vulkan1.2',
                   '-fvk-use-dx-layout', '-O3'] + ['-D' + d for d in defines] + ['-Fo', spv, h])
    if rc:
        return fail('dxc', log)
    gl = os.path.join(work, 'glsl', name + '.glsl')
    rc, log = run([CROSS, spv, '--version', '460', '--no-es', '--output', gl])
    if rc:
        return fail('cross', log)
    glsl = open(gl).read()
    for ext in FORBIDDEN:
        if ext in glsl:
            return fail('ext', 'forbidden extension ' + ext)
    if 'SPIRV_CROSS_CONSTANT_ID' in glsl:
        return fail('spec', 'a specialization constant is left (shader_common.h, SDNX_DEKO3D)')
    m = DYNAMIC_COMPONENT_RE.search(glsl)
    if m:
        return fail('ubo', 'dynamic component index into a UBO vector: ' + m.group(0))
    try:
        glsl, table = bind_textures(glsl)
    except Exception as e:
        return fail('bind', str(e))
    open(gl, 'w').write(glsl)
    out = os.path.join(work, name + '.dksh')
    rc, log = run([UAM, '-s', stage, '-o', out, gl])
    if rc or not os.path.exists(out):
        return fail('uam', log)
    return name, 'ok', '', table


def main():
    vk_pkg, hlsl_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
    jobs = int(sys.argv[4]) if len(sys.argv) > 4 else os.cpu_count()
    internal = os.path.join(HERE, '..', '..', '..', 'port', 'shaders', 'internal')
    entries = [l.rstrip('\n') for l in open(os.path.join(vk_pkg, 'index.txt')) if l.strip()]
    work = out + '.work'
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(out)

    def source(name):
        # <name>.xenos.hlsl: an internal shader the Vulkan package has only as SPIR-V, translated from its container
        # by the current translator (port/shaders/internal).
        for p in (os.path.join(hlsl_dir, name + '.hlsl'), os.path.join(internal, name + '.hlsl'),
                  os.path.join(internal, name + '.xenos.hlsl')):
            if os.path.exists(p):
                return p
        return None

    def job(line):
        kind, key, name = line.split()[:3]
        src = source(name)
        if not src:
            return name, 'source', 'no HLSL in %s or %s' % (hlsl_dir, internal), None
        return compile_one(src, work, 'frag' if kind == 'ps' else 'vert', name=name)
    with ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(job, entries))
    index, textures, failed = [], [], []
    for line, (name, status, why, table) in zip(entries, results):
        if status != 'ok':
            failed.append((name, status, why))
            continue
        shutil.move(os.path.join(work, name + '.dksh'), os.path.join(out, name + '.dksh'))
        index.append(line)
        textures.append(' '.join([name] + ['%d:%s:%d' % t for t in table]))
    open(os.path.join(out, 'index.txt'), 'w').write('\n'.join(index) + '\n')
    open(os.path.join(out, 'textures.txt'), 'w').write('\n'.join(textures) + '\n')
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print('deko3d package: %d of %d shaders, %.1f MB -> %s' % (len(index), len(entries), size / 1e6, out))
    for name, status, why in failed[:20]:
        print('  FAILED %s: %s: %s' % (name, status, why))
    if len(failed) > 20:
        print('  ... %d more' % (len(failed) - 20))
    print('work files (HLSL, SPIR-V, GLSL, errors): ' + work)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
