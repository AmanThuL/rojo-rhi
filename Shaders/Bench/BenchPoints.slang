//----------------------------------------------------------------------------------------------------------------------
/// @file BenchPoints.slang
/// @brief Benchmark shader: one point at pixel drawId of a 256 by 256 target, filled with drawId + 1 as a float.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

static const uint kTargetSize = 256;

struct PointOutput
{
    float4 position : SV_Position;
    float pointSize : SV_PointSize;
    nointerpolation uint drawId : DRAWID;
};

struct PointInput
{
    float4 position : SV_Position;
    nointerpolation uint drawId : DRAWID;
};

[shader("vertex")]
PointOutput pointVertex(rojoRHI::VertexIds ids)
{
    const uint drawId = rojoRHI::drawId(ids);
    const float2 pixel = float2(drawId % kTargetSize, drawId / kTargetSize) + 0.5;
    const float2 ndc = pixel / float(kTargetSize) * 2.0 - 1.0;
    PointOutput output;
    output.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    output.pointSize = 1.0;
    output.drawId = drawId;
    return output;
}

[shader("fragment")]
float pointFragment(PointInput input) : SV_Target
{
    return float(input.drawId + 1);
}
