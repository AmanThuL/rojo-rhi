//----------------------------------------------------------------------------------------------------------------------
/// @file DrawOracle.slang
/// @brief Conformance draw oracle: a vertex entry that places one small triangle over a pixel of a 256 by 256 target
/// chosen by the draw identifier and the instance, a fragment entry that writes what the vertex stage saw, and a
/// fullscreen triangle with a solid color. No entry touches a reserved binding, so no entry gets a buffer parameter.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// The target's edge in pixels, and the cells one instance advances: one instance of 65,536 draws, or four instances of
// 16,384, cover the target once.
static const uint kTargetEdge = 256;
static const uint kInstanceCells = 16384;

// What the oracle passes to a fragment entry. `drawId` and `uv` match the inputs `AbiFragment.slang` reads.
struct OracleVaryings
{
    float4 position : SV_Position;
    nointerpolation uint drawId : DRAWID;
    float2 uv : TEXCOORD0;
    nointerpolation uint4 ids : ORACLEIDS;
};

// The clip-space position of `corner` of the triangle over pixel `cell`. The corners lie 0.4 pixel left of and above
// the pixel's centre, then 0.8 pixel right of it, then 0.8 pixel below it, so the triangle covers that centre and no
// other; in clip space, whose y points up, they wind clockwise.
float4 cellCorner(uint cell, uint corner)
{
    float2 pixel = float2(float(cell % kTargetEdge) + 0.5, float(cell / kTargetEdge) + 0.5);
    if (corner == 0)
    {
        pixel += float2(-0.4, -0.4);
    }
    else if (corner == 1)
    {
        pixel += float2(0.8, -0.4);
    }
    else
    {
        pixel += float2(-0.4, 0.8);
    }
    float scale = 2.0 / float(kTargetEdge);
    return float4(pixel.x * scale - 1.0, 1.0 - pixel.y * scale, 0.0, 1.0);
}

[shader("vertex")]
OracleVaryings oracleVertexMain(rojoRHI::VertexIds ids)
{
    uint vertex = rojoRHI::vertexIndex(ids);
    uint instance = rojoRHI::instanceIndex(ids);
    uint drawId = rojoRHI::drawId(ids);
    uint corner = vertex % 3;
    OracleVaryings output;
    output.position = cellCorner(drawId + kInstanceCells * instance, corner);
    output.drawId = drawId;
    output.uv = float2(0.5, 0.5);
    // A flat value comes from the triangle's first vertex, so the third component is that vertex's index rounded
    // down to a multiple of 3 when the triangle's vertices are consecutive.
    output.ids = uint4(drawId, instance, vertex - corner, 0);
    return output;
}

[shader("fragment")]
float4 oracleFragmentMain(OracleVaryings input) : SV_Target
{
    return float4(input.ids + 1);
}

[shader("vertex")]
float4 fullscreenVertexMain(rojoRHI::VertexIds ids) : SV_Position
{
    // One triangle over the whole target, at depth 0.5.
    uint corner = rojoRHI::vertexIndex(ids) % 3;
    float2 uv = float2(float((corner << 1) & 2), float(corner & 2));
    return float4(uv * 2.0 - 1.0, 0.5, 1.0);
}

[shader("fragment")]
float4 solidFragmentMain() : SV_Target
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
