# Debug variant of the final composite pixel shader ps_195084cd918f58f5 (native renderer gameplay frame is near
# black). Writes port/shots/ps_195084cd918f58f5_debug.hlsl: the screen is split into a 3x2 grid, each cell shows an
# intermediate value instead of the result:
#   top:    pre-tonemap colour c | lit colour (r4) | fog rgb
#   bottom: fog alpha            | diffuse texture | ambient texture
# mode 1 (argument 1): collector rgb | collector alpha | ambient2 rgb / ambient2 alpha | diffuse2 | cbEnvironmentSettings(3)
# mode 2: ambient decode r12.x | collector.z | r4 after r12*r7 / r5.yzw | r10.xyz | r11.xyz
# mode 3: diffuse2 everywhere; mode 4: diffuse2 x1, /32, /1024 / ambient | lit | colour
# mode 5: screen position gradient (checks that the debug shader is what runs)
# mode 10 + m: the cells of mode m as six full-width horizontal bands of 118 rows (vertical seams per value)
# Usage: python debug_composite.py  (then compile it in WSL and swap the .spv in the package, see portlog 16.28)
import re

src = r'D:\CODE\SDNX\port\shots\ps_195084cd918f58f5.hlsl'
import sys
MODE = int(sys.argv[1]) if len(sys.argv) > 1 else 0
dst = r'D:\CODE\SDNX\port\shots\ps_195084cd918f58f5_debug%d.hlsl' % MODE
s = open(src, encoding='utf-8').read()


def after(pattern, text):
    global s
    m = re.search(pattern, s)
    assert m, pattern
    line_end = s.index('\n', m.end())
    s = s[:line_end + 1] + '\t' + text + '\n' + s[line_end + 1:]


after(r'r8\.xyz = tfetch2D\(_texDiffuse_Texture2D', 'float3 dDiffuse = r8.xyz;')
after(r'r7\.w = tfetch2D\(_texCollector_Texture2D', 'float4 dCollector = tfetch2D(_texCollector_Texture2DDescriptorIndex, _texCollector_SamplerDescriptorIndex, r4.xy, float2(0, 0), _texCollector_InvSize);')
after(r'r10\.xyzw = tfetch2D\(_texAmbient2_Texture2D', 'float4 dAmbient2 = r10;')
after(r'r11\.xyz = tfetch2D\(_texDiffuse2_Texture2D', 'float3 dDiffuse2 = r11.xyz;')
after(r'r5\.xyz = tfetch2D\(_texAmbient_Texture2D', 'float3 dAmbient = r5.xyz;')
after(r'r1\.xyzw = tfetch3D\(_texFogCube', 'float4 dFog = r1;')
after(r'r0\.xyz = r4\.xyz \* r0\.www \+ r2\.xyz;', 'float3 dLit = r4.xyz;')
after(r'r1\.xyz = r4\.xyz \* r0\.www \+ r1\.xyz;', 'float3 dColour = r1.xyz;')
after(r'r12\.x = r8\.w \* r2\.w;', 'float dAmbDecode = r12.x;')
after(r'r4\.xyzw = r12\.xyzw \* r7\.wyww;', 'float3 dR4 = r4.xyz; float dCollZ = r7.w;')
after(r'r5\.yzw = r10\.xyz \* r9\.xyz \+ r11\.xyz;', 'float3 dR5 = r5.yzw; float3 dR10 = r10.xyz; float3 dR11 = r11.xyz;')
final = 'oC0.xyz = r1.xyz * r0.xyz;'
assert s.count(final) == 1
s = s.replace(final, final + '''
	{
		uint cx = uint(iPos.x) / 400u, cy = uint(iPos.y) / 352u;
		uint cell = min(cx, 2u) + 3u * min(cy, 1u);
		if (%d >= 10) cell = min(uint(iPos.y) / 118u, 5u);  // bands: every value spans the full width
		float3 d;
		if (%d == 5) d = float3(iPos.x / 1200.0, iPos.y / 704.0, 0.0);
		else if (%d == 3) d = dDiffuse2;
		else if (%d == 4) d = cell == 0u ? dDiffuse2 : cell == 1u ? dDiffuse2 / 32.0 : cell == 2u ? dDiffuse2 / 1024.0 :
		                   cell == 3u ? dAmbient : cell == 4u ? dLit : dColour;
		else if (%d == 0) d = cell == 0u ? dColour : cell == 1u ? dLit : cell == 2u ? dFog.rgb :
		                   cell == 3u ? dFog.aaa : cell == 4u ? dDiffuse : dAmbient;
		else if (%d == 2) d = cell == 0u ? dAmbDecode.xxx : cell == 1u ? dCollZ.xxx : cell == 2u ? dR4 :
		                        cell == 3u ? dR5 : cell == 4u ? dR10 : dR11;
		else d = cell == 0u ? dCollector.rgb : cell == 1u ? dCollector.aaa : cell == 2u ? dAmbient2.rgb :
		         cell == 3u ? dAmbient2.aaa : cell == 4u ? dDiffuse2 : cbEnvironmentSettings(3).xyz;
		oC0 = float4(d, 1.0);
	}''' % ((MODE,) + (MODE % 10,) * 5))
open(dst, 'w', encoding='utf-8', newline='\n').write(s)
print('ok', dst)
