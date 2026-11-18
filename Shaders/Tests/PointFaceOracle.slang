//----------------------------------------------------------------------------------------------------------------------
/// @file PointFaceOracle.slang
/// @brief Point face oracle: one point at the centre pixel of a 16 by 16 viewport whose fragment writes the face it is
/// rasterized as to target 0, and a fullscreen triangle that writes a solid color to target 1. No entry touches a
/// reserved binding, so no entry gets a buffer parameter.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct PointVaryings
{
    float4 position : SV_Position;
    // D3D12 rasterizes every point as one pixel and has no point size; DXIL refuses SV_PointSize in a vertex shader.
#if !defined(ROJORHI_TARGET_HLSL)
    float pointSize : SV_PointSize;
#endif
};

struct FaceTargets
{
    float4 face : SV_Target0;
    float4 probe : SV_Target1;
};

[shader("vertex")]
PointVaryings pointFaceVertexMain()
{
    // A sixteenth of the viewport right of and below its centre, which is the centre of pixel (8, 8) of a 16 by 16
    // viewport; in clip space y points up.
    PointVaryings output;
    output.position = float4(1.0 / 16.0, -1.0 / 16.0, 0.0, 1.0);
#if !defined(ROJORHI_TARGET_HLSL)
    output.pointSize = 1.0;
#endif
    return output;
}

[shader("fragment")]
FaceTargets pointFaceFragmentMain(bool front : SV_IsFrontFace)
{
    // Red for a front face, green for a back face.
    FaceTargets targets;
    targets.face = front ? float4(1.0, 0.0, 0.0, 1.0) : float4(0.0, 1.0, 0.0, 1.0);
    targets.probe = float4(0.0, 0.0, 0.0, 0.0);
    return targets;
}

[shader("vertex")]
float4 probeVertexMain(rojoRHI::VertexIds ids) : SV_Position
{
    // One triangle over the whole viewport.
    uint corner = rojoRHI::vertexIndex(ids) % 3;
    float2 uv = float2(float((corner << 1) & 2), float(corner & 2));
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
FaceTargets probeFragmentMain()
{
    // Blue in target 1, where the stencil test passes.
    FaceTargets targets;
    targets.face = float4(0.0, 0.0, 0.0, 0.0);
    targets.probe = float4(0.0, 0.0, 1.0, 1.0);
    return targets;
}
