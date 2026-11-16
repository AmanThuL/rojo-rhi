//----------------------------------------------------------------------------------------------------------------------
/// @file PullOracle.slang
/// @brief Conformance pull oracle: a vertex entry that pulls its clip-space position from the address its root names,
/// a fragment entry that writes a constant color, and a compute entry that copies `uint4` elements from one address to
/// another, the producer the barrier cases order before a consumer. The vertex entry reads the root and returns a
/// position, so the Metal compiler prints its "writable resources in non-void vertex function" warning for it.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct PullRoot
{
    rojoRHI::Addr vertices;
};

struct CopyRoot
{
    rojoRHI::Addr source;
    rojoRHI::Addr destination;
    uint count;
    uint pad;
};

[shader("vertex")]
float4 pullVertexMain(rojoRHI::VertexIds ids) : SV_Position
{
    PullRoot root = rojoRHI::root<PullRoot>();
    return rojoRHI::load<float4>(root.vertices, rojoRHI::vertexIndex(ids));
}

[shader("fragment")]
float4 pullFragmentMain() : SV_Target
{
    return float4(0.25, 0.5, 0.75, 1.0);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void copyMain(uint3 id : SV_DispatchThreadID)
{
    CopyRoot root = rojoRHI::root<CopyRoot>();
    if (id.x < root.count)
    {
        rojoRHI::store<uint4>(root.destination, id.x, rojoRHI::load<uint4>(root.source, id.x));
    }
}
