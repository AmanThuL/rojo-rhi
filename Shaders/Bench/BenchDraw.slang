//----------------------------------------------------------------------------------------------------------------------
/// @file BenchDraw.slang
/// @brief C1 and C2: a one-pixel triangle per draw at the pixel its root names, filled with the root's color.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// Mirrors DrawRoot in Benchmarks/Common/CpuRoots.h: 64 bytes.
struct DrawRoot
{
    uint index;
    uint reserved0;
    uint reserved1;
    uint reserved2;
    float4 color;
    uint4 reserved3;
    uint4 reserved4;
};

struct DrawOutput
{
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
};

// In pixels from the pixel's top-left corner: the triangle holds the pixel's centre and no other pixel's.
static const float2 kCorners[3] = { float2(0.0, 0.0), float2(1.5, 0.0), float2(0.0, 1.5) };

[shader("vertex")]
DrawOutput drawVertex(rojoRHI::VertexIds ids)
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    uint pixel = root.index % 65536;
    float2 corner = float2(float(pixel % 256), float(pixel / 256)) + kCorners[rojoRHI::vertexIndex(ids)];
    DrawOutput output;
    // Row 0 of the 256x256 target is its top row.
    output.position = float4(corner.x / 128.0 - 1.0, 1.0 - corner.y / 128.0, 0.0, 1.0);
    output.color = root.color;
    return output;
}

[shader("fragment")]
float4 drawFragment(DrawOutput input) : SV_Target
{
    return input.color;
}
