//----------------------------------------------------------------------------------------------------------------------
/// @file AbiVertex.slang
/// @brief Compile-only conformance shader: the three vertex-stage built-ins, with varyings.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct VertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint drawId : DRAWID;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(rojoRHI::VertexIds ids)
{
    VertexOutput output;
    output.position = float4(float(rojoRHI::vertexIndex(ids)), 0, 0, 1);
    output.drawId = rojoRHI::drawId(ids);
    output.uv = float2(float(rojoRHI::instanceIndex(ids)), 0);
    return output;
}
