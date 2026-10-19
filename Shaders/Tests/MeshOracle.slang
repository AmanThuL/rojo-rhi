//----------------------------------------------------------------------------------------------------------------------
/// @file MeshOracle.slang
/// @brief Conformance mesh oracle: each task group passes the draw identifier and its group index to one mesh group,
/// which places one small triangle over a pixel of a 256 by 256 target chosen by both, and the fragment entry writes
/// the identifier and the group's x. A mesh entry without a task stage does the same from its own group. The task and
/// mesh entries read the draw's record at `buffer(3)`, so every entry of the file receives it.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// The target's edge in pixels, the cells one step of a group's x advances, and those one step of `y + 2 z` advances,
// which keeps the cells of a group distinct while x is below 4, y and z below 2 and the draw identifier below 4096.
static const uint kTargetEdge = 256;
static const uint kGroupCells = 16384;
static const uint kPlaneCells = 4096;

struct OraclePayload
{
    uint drawId;
    uint3 group;
};

struct OracleMeshVertex
{
    float4 position : SV_Position;
};

// The identifiers go per primitive: the pinned Slang drops a user semantic from a mesh vertex output in MSL, so a
// fragment input could not match it, and keeps it on a primitive output.
struct OraclePrimitive
{
    nointerpolation uint2 ids : ORACLEIDS;
};

struct OracleFragmentInput
{
    float4 position : SV_Position;
    nointerpolation uint2 ids : ORACLEIDS;
};

// The pixel the triangle of `group` of a draw with `drawId` covers.
uint cellOf(uint drawId, uint3 group)
{
    return drawId + kGroupCells * group.x + kPlaneCells * (group.y + 2 * group.z);
}

// The clip-space position of `corner` of the triangle over pixel `cell`, as in `DrawOracle.slang`: it covers that
// pixel's centre and no other, winding clockwise in clip space.
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
void oracleTaskMain(uint3 group : SV_GroupID)
{
    OraclePayload payload;
    payload.drawId = rojoRHI::drawId();
    payload.group = group;
    DispatchMesh(1, 1, 1, payload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void oracleMeshMain(
    in payload OraclePayload payload,
    out vertices OracleMeshVertex vertices[3],
    out indices uint3 triangles[1],
    out primitives OraclePrimitive primitives[1])
{
    SetMeshOutputCounts(3, 1);
    uint cell = cellOf(payload.drawId, payload.group);
    for (uint corner = 0; corner < 3; ++corner)
    {
        OracleMeshVertex vertex;
        vertex.position = cellCorner(cell, corner);
        vertices[corner] = vertex;
    }
    triangles[0] = uint3(0, 1, 2);
    OraclePrimitive primitive;
    primitive.ids = uint2(payload.drawId, payload.group.x);
    primitives[0] = primitive;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void oracleMeshOnlyMain(
    uint3 group : SV_GroupID,
    out vertices OracleMeshVertex vertices[3],
    out indices uint3 triangles[1],
    out primitives OraclePrimitive primitives[1])
{
    SetMeshOutputCounts(3, 1);
    uint drawId = rojoRHI::drawId();
    uint cell = cellOf(drawId, group);
    for (uint corner = 0; corner < 3; ++corner)
    {
        OracleMeshVertex vertex;
        vertex.position = cellCorner(cell, corner);
        vertices[corner] = vertex;
    }
    triangles[0] = uint3(0, 1, 2);
    OraclePrimitive primitive;
    primitive.ids = uint2(drawId, group.x);
    primitives[0] = primitive;
}

[shader("fragment")]
float4 meshFragmentMain(OracleFragmentInput input) : SV_Target
{
    return float4(float(input.ids.x + 1), float(input.ids.y + 1), 0.0, 1.0);
}
