# Debug variant of a shadow collector pixel shader (the light volumes of the deferred lighting, which compare the
# shadow map with the light-space depth of the scene): writes the compared values instead of the collector output.
#   python debug_shadow.py <shader name> <out.hlsl>
#   R = frac(16 x light-space depth of the pixel minus the bias), A = floor(16 x it) / 16 (depth = A + R / 16)
#   G = frac(16 x the shadow map at the centre tap)
#   B = (map - depth) x 50 + 0.5: above 0.5 lit, below shadowed
# The HLSL comes from ~/sd-shaders/hlsl (WSL, the shader library build).
import re
import subprocess
import sys

name, out = sys.argv[1], sys.argv[2]
# mode "uv" (third argument) with a marker (fourth, 0..1): R, G = the centre tap's map coordinates, B = the marker
# (which cascade wrote the pixel).
MODE = sys.argv[3] if len(sys.argv) > 3 else 'compare'
MARKER = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
src = subprocess.run(['wsl', '-e', 'bash', '-lc', 'cat ~/sd-shaders/hlsl/%s.hlsl' % name], capture_output=True,
                     text=True, encoding='utf-8').stdout
assert src, 'no HLSL for ' + name


def after(pattern, text):
    global src
    m = re.search(pattern, src)
    assert m, pattern
    end = src.index('\n', m.end())
    src = src[:end + 1] + '\t' + text + '\n' + src[end + 1:]


m = re.search(r'(r\d+\.x) = r\d+\.x \+ -cbShadowTransform\(13\)\.\w;', src)
assert m, 'no bias subtraction'
after(re.escape(m.group(0)), 'float dDepth = %s;' % m.group(1))
m = re.search(r'(r\d+\.\w) = (?:SDNX_TEX\(tfetch2D, \d+, |tfetch2D\()_texShadowAtlas_Texture2DDescriptorIndex[^\n]*r\d+\.zy,'
              r'[^\n]*', src)
assert m, 'no centre tap'
after(re.escape(m.group(0)), 'float dMap = %s;' % m.group(1))
uv = re.search(r'texShadowAtlas_SamplerDescriptorIndex, (r\d+\.zy),', m.group(0)).group(1)
src = src.replace(m.group(0), m.group(0) + '\n\tfloat2 dUV = %s;' % uv, 1)
m = re.search(r'(r\d+\.z) = r1\.w \* r1\.z;', src)
assert m, 'no view depth'
after(re.escape(m.group(0)), 'float dView = %s;' % m.group(1))
final = re.search(r'\toC0\.xyzw = r\d+\.xyzw;\n', src)
assert final, 'no final write'
debug = ('\toC0 = float4(frac(dDepth * 16.0), frac(dMap * 16.0), saturate((dMap - dDepth) * 50.0 + 0.5), '
         'floor(dDepth * 16.0) / 16.0);\n')
if MODE == 'uv':
    debug = '\toC0 = float4(dUV, %f, 1.0);\n' % MARKER
src = src[:final.end()] + debug + src[final.end():]
open(out, 'w', encoding='utf-8', newline='\n').write(src)
print('ok', out)
