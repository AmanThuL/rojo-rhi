//----------------------------------------------------------------------------------------------------------------------
/// @file AbiMesh.slang
/// @brief Compile-only conformance pair: `drawId` from a task and a mesh entry point.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct TaskPayload
{
    uint drawId;
};

struct MeshVertex
{
    float4 position : SV_Position;
};

[shader("amplification")]
[numthreads(1, 1, 1)]
void taskMain()
{
    TaskPayload payload;
    payload.drawId = rojoRHI::drawId();
    DispatchMesh(1, 1, 1, payload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain(in payload TaskPayload payload, out vertices MeshVertex vertices[3], out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    MeshVertex vertex;
    vertex.position = float4(float(rojoRHI::drawId() + payload.drawId), 0, 0, 1);
    vertices[0] = vertex;
    vertices[1] = vertex;
    vertices[2] = vertex;
    triangles[0] = uint3(0, 1, 2);
}
