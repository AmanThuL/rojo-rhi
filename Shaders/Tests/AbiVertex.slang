//----------------------------------------------------------------------------------------------------------------------
/// @file AbiVertex.slang
/// @brief Conformance vertex entry that stores what it was given: it writes `(vertexIndex, instanceIndex, drawId)`
/// at a position derived from the three built-ins, so a rasterization-disabled draw can be read back. The function
/// is void, which Metal accepts with writable resources; the fragment entry lives in `AbiFragment.slang`.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct VertexRoot
{
    rojoRHI::Addr output;
    uint firstVertex;
    uint verticesPerInstance;
};

[shader("vertex")]
void vertexMain(rojoRHI::VertexIds ids)
{
    VertexRoot root = rojoRHI::root<VertexRoot>();
    uint vertex = rojoRHI::vertexIndex(ids);
    uint instance = rojoRHI::instanceIndex(ids);
    uint index = instance * root.verticesPerInstance + vertex - root.firstVertex;
    rojoRHI::store<uint4>(root.output, index, uint4(vertex, instance, rojoRHI::drawId(ids), 0));
}
