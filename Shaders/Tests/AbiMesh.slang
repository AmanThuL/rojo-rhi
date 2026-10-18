//----------------------------------------------------------------------------------------------------------------------
/// @file AbiMesh.slang
/// @brief Conformance pair: `drawId` from a task and a mesh entry point; the mesh entry runs three threads, each
/// writing its own vertex, so a packaged size other than 1 1 1 is exercised.
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
[numthreads(3, 1, 1)]
void meshMain(
    uint thread : SV_GroupThreadID,
    in payload TaskPayload payload,
    out vertices MeshVertex vertices[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    MeshVertex vertex;
    vertex.position = float4(float(rojoRHI::drawId() + payload.drawId), float(thread), 0, 1);
    vertices[thread] = vertex;
    if (thread == 0)
    {
        triangles[0] = uint3(0, 1, 2);
    }
}
