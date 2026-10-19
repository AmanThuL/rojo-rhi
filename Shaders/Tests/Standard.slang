//----------------------------------------------------------------------------------------------------------------------
/// @file Standard.slang
/// @brief The standard library of the contract cases that load a shader library: one entry of each stage, none of them
/// touching a reserved binding (no `root`, `load`, `store`, texture, sampler or `drawId` call), so that Slang gives no
/// entry a buffer parameter and every entry can sit in a pipeline whatever its stage.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct TaskPayload
{
    uint value;
};

struct MeshVertex
{
    float4 position : SV_Position;
};

[shader("vertex")]
float4 vsMain(rojoRHI::VertexIds ids) : SV_Position
{
    uint corner = rojoRHI::vertexIndex(ids) % 3;
    float2 uv = float2(float((corner << 1) & 2), float(corner & 2));
    return float4(uv * 2.0 - 1.0, 0, 1);
}

[shader("fragment")]
float4 fsMain() : SV_Target
{
    return float4(0.25, 0.5, 0.75, 1.0);
}

[shader("amplification")]
[numthreads(1, 1, 1)]
void tsMain()
{
    TaskPayload payload;
    payload.value = 1;
    DispatchMesh(1, 1, 1, payload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void msMain(
    in payload TaskPayload payload,
    out vertices MeshVertex vertices[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    MeshVertex corners[3];
    corners[0].position = float4(-1, -1, 0, 1);
    corners[1].position = float4(3, -1, 0, 1);
    corners[2].position = float4(-1, 3, 0, 1);
    vertices[0] = corners[0];
    vertices[1] = corners[1];
    vertices[2] = corners[2];
    triangles[0] = uint3(0, 1, 2);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
}
