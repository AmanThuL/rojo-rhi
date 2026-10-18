//----------------------------------------------------------------------------------------------------------------------
/// @file BenchPlain.slang
/// @brief C3 and C8: a fixed triangle at depth 0.5 in a constant color, reading no reserved binding.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// Counter-clockwise in clip space; no pixel centre of the 256x256 target lies on an edge.
static const float2 kCorners[3] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5) };

[shader("vertex")]
float4 plainVertex(rojoRHI::VertexIds ids) : SV_Position
{
    return float4(kCorners[rojoRHI::vertexIndex(ids)], 0.5, 1.0);
}

// 51, 102, 153 and 255 of 255: exact in RGBA8Unorm.
[shader("fragment")]
float4 plainFragment() : SV_Target
{
    return float4(0.2, 0.4, 0.6, 1.0);
}
