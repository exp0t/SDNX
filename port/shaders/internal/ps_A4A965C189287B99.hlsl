// SDNX: replacement for the run-time Direct3D clear pixel shader A4A965C189287B99 (9 words: one ALU export,
// oC0 = r0 = interpolator 0). The colour comes from vs_477B525CE86A19A5 (the clear colour of the vertices,
// TEXCOORD0 = location 0). The earlier hand-built version read no interpolator at all (only FragCoord /
// FrontFacing) and filled the clears with a position-dependent colour (green-yellow, split along the diagonal).
float4 main(float4 iTexCoord0 : TEXCOORD0) : SV_Target0
{
    return iTexCoord0;
}
