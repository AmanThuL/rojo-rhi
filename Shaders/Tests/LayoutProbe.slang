//----------------------------------------------------------------------------------------------------------------------
/// @file LayoutProbe.slang
/// @brief Layout probe: mirrors the shared C++ structs so the build can compare their reflected offsets.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;
import AtomicTest;

// A parameter named gRecord_<Name> mirrors the C++ struct rojoRHI::<Name> in natural layout. A parameter named
// gRoot_<Name> mirrors a root block and must also satisfy the flat root rules. Tools/shader_layout.py reads them.

struct DrawIndirectArgs
{
    uint drawId;
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
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

struct DrawMeshTasksIndirectArgs
{
    uint drawId;
    uint x;
    uint y;
    uint z;
};

struct DispatchIndirectArgs
{
    uint x;
    uint y;
    uint z;
};

struct DrawRoot
{
    rojoRHI::Addr vertices;
    rojoRHI::Addr instances;
    uint albedo;
    uint linearClamp;
    uint drawId;
    uint pad;
};

StructuredBuffer<DrawIndirectArgs> gRecord_DrawIndirectArgs;
StructuredBuffer<DrawIndexedIndirectArgs> gRecord_DrawIndexedIndirectArgs;
StructuredBuffer<DrawMeshTasksIndirectArgs> gRecord_DrawMeshTasksIndirectArgs;
StructuredBuffer<DispatchIndirectArgs> gRecord_DispatchIndirectArgs;
StructuredBuffer<DrawRoot> gRoot_DrawRoot;
StructuredBuffer<AtomicRoot> gRoot_AtomicRoot;

// Reflection lists every global parameter whether or not the entry reads it; the entry only gives each target
// something to compile.
[shader("compute")]
[numthreads(1, 1, 1)]
void probeMain()
{
}
