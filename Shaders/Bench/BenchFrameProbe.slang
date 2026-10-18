//----------------------------------------------------------------------------------------------------------------------
/// @file BenchFrameProbe.slang
/// @brief Layout probe: mirrors the composed frame's roots and records so the build compares their reflected offsets.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// A parameter named gRecord_<Name> mirrors the C++ struct rojoRHI::bench::<Name> in natural layout. A parameter named
// gRoot_<Name> mirrors a root block and must also satisfy the flat root rules. Tools/shader_layout.py reads them. Each
// definition below is the text of the struct in the entry file that uses it.

struct Transform
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
};

struct MeshVertex
{
    float4 position;
    float4 normal;
};

struct DrawIndexedIndirectArgs
{
    uint drawId;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int baseVertex;
    uint firstInstance;
};

struct CullRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr records0;
    rojoRHI::Addr records1;
    rojoRHI::Addr records2;
    rojoRHI::Addr records3;
    rojoRHI::Addr counts0;
    rojoRHI::Addr counts1;
    rojoRHI::Addr counts2;
    rojoRHI::Addr counts3;
    uint pad0;
    uint pad1;
};

struct GeometryRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr vertices;
    float4 viewProj0;
    float4 viewProj1;
    float4 viewProj2;
    float4 viewProj3;
};

struct ShadowRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr vertices;
    float4 viewProj0;
    float4 viewProj1;
    float4 viewProj2;
    float4 viewProj3;
};

struct AoRoot
{
    uint depth;
    uint normal;
    uint aoRaw;
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct BlurRoot
{
    uint aoRaw;
    uint ao;
    uint width;
    uint height;
};

struct LightingRoot
{
    uint albedo;
    uint normal;
    uint material;
    uint depth;
    uint shadow;
    uint ao;
    uint hdr;
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 lightDirection;
    float4 inverseViewProj0;
    float4 inverseViewProj1;
    float4 inverseViewProj2;
    float4 inverseViewProj3;
    float4 cascade0Row0;
    float4 cascade0Row1;
    float4 cascade0Row2;
    float4 cascade0Row3;
    float4 cascade1Row0;
    float4 cascade1Row1;
    float4 cascade1Row2;
    float4 cascade1Row3;
    float4 cascade2Row0;
    float4 cascade2Row1;
    float4 cascade2Row2;
    float4 cascade2Row3;
};

struct ForwardRoot
{
    float4 transform0;
    float4 transform1;
    float4 transform2;
    float4 transform3;
    float4 color;
    rojoRHI::Addr vertices;
    rojoRHI::Addr camera;
};

struct BloomDownRoot
{
    uint source;
    uint samplerId;
    float texelX;
    float texelY;
};

struct BloomUpRoot
{
    uint source;
    uint samplerId;
    float intensity;
    uint pad0;
};

struct ToneMapRoot
{
    uint source;
    uint samplerId;
    float exposure;
    uint pad0;
};

StructuredBuffer<Transform> gRecord_Transform;
StructuredBuffer<MeshVertex> gRecord_MeshVertex;
StructuredBuffer<DrawIndexedIndirectArgs> gRecord_DrawIndexedIndirectArgs;
StructuredBuffer<CullRoot> gRoot_CullRoot;
StructuredBuffer<GeometryRoot> gRoot_GeometryRoot;
StructuredBuffer<ShadowRoot> gRoot_ShadowRoot;
StructuredBuffer<AoRoot> gRoot_AoRoot;
StructuredBuffer<BlurRoot> gRoot_BlurRoot;
StructuredBuffer<LightingRoot> gRoot_LightingRoot;
StructuredBuffer<ForwardRoot> gRoot_ForwardRoot;
StructuredBuffer<BloomDownRoot> gRoot_BloomDownRoot;
StructuredBuffer<BloomUpRoot> gRoot_BloomUpRoot;
StructuredBuffer<ToneMapRoot> gRoot_ToneMapRoot;

// Reflection lists every global parameter whether or not the entry reads it; the entry only gives each target
// something to compile.
[shader("compute")]
[numthreads(1, 1, 1)]
void probeMain()
{
}
