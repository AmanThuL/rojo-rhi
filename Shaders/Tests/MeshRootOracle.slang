//----------------------------------------------------------------------------------------------------------------------
/// @file MeshRootOracle.slang
/// @brief Conformance mesh oracle that reads the root in each stage: the task entry samples a view through a sampler
/// at the texel the draw identifier chooses and adds a bias from the root, the mesh entry shifts its pixel by a root
/// value, and the fragment entry adds a third. So an entry sees what the backend binds at `buffer(0)` to `buffer(3)` of
/// its own stage. Each task group places one small triangle over a pixel of a 256 by 256 target, as `MeshOracle` does.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// The target's edge in pixels, the cells one task group advances, and the texels of the sampled row.
static const uint kTargetEdge = 256;
static const uint kGroupCells = 16384;
static const uint kRowTexels = 16;

struct MeshRoot
{
    uint view;         // A 16 by 1 `R32Float` view whose texel `i` holds a whole number.
    uint sampler;      // A nearest sampler.
    uint taskBias;     // Added by the task entry to the sampled value.
    uint meshShift;    // Added by the mesh entry to the pixel's cell.
    uint fragmentBias; // Added by the fragment entry to the group.
    uint pad0;
    uint pad1;
    uint pad2;
};

struct RootPayload
{
    uint drawId;
    uint group;
    uint value;
};

struct RootMeshVertex
{
    float4 position : SV_Position;
};

// The values go per primitive, as in `MeshOracle.slang`: the pinned Slang drops a user semantic from a mesh vertex
// output in MSL.
struct RootPrimitive
{
    nointerpolation uint3 ids : ORACLEIDS;
};

struct RootFragmentInput
{
    float4 position : SV_Position;
    nointerpolation uint3 ids : ORACLEIDS;
};

// The clip-space position of `corner` of the triangle over pixel `cell`, as in `MeshOracle.slang`.
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

[shader("amplification")]
[numthreads(1, 1, 1)]
void rootTaskMain(uint3 group : SV_GroupID)
{
    MeshRoot root = rojoRHI::root<MeshRoot>();
    uint drawId = rojoRHI::drawId();
    float2 uv = float2((float(drawId % kRowTexels) + 0.5) / float(kRowTexels), 0.5);
    float texel = rojoRHI::texture2D(root.view).SampleLevel(rojoRHI::sampler(root.sampler), uv, 0.0).x;
    RootPayload payload;
    payload.drawId = drawId;
    payload.group = group.x;
    payload.value = root.taskBias + uint(texel);
    DispatchMesh(1, 1, 1, payload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void rootMeshMain(
    in payload RootPayload payload,
    out vertices RootMeshVertex vertices[3],
    out indices uint3 triangles[1],
    out primitives RootPrimitive primitives[1])
{
    SetMeshOutputCounts(3, 1);
    MeshRoot root = rojoRHI::root<MeshRoot>();
    uint cell = payload.drawId + root.meshShift + kGroupCells * payload.group;
    for (uint corner = 0; corner < 3; ++corner)
    {
        RootMeshVertex vertex;
        vertex.position = cellCorner(cell, corner);
        vertices[corner] = vertex;
    }
    triangles[0] = uint3(0, 1, 2);
    RootPrimitive primitive;
    primitive.ids = uint3(payload.drawId, payload.value, payload.group);
    primitives[0] = primitive;
}

[shader("fragment")]
float4 rootFragmentMain(RootFragmentInput input) : SV_Target
{
    MeshRoot root = rojoRHI::root<MeshRoot>();
    return float4(float(input.ids.x + 1), float(input.ids.y), float(input.ids.z + 1 + root.fragmentBias), 1.0);
}
