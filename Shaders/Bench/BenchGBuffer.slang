//----------------------------------------------------------------------------------------------------------------------
/// @file BenchGBuffer.slang
/// @brief The composed frame's geometry pass: one box per draw record into the albedo, normal and material targets.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

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

struct GeometryRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr vertices;
    float4 viewProj0;
    float4 viewProj1;
    float4 viewProj2;
    float4 viewProj3;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    nointerpolation uint drawId : DRAWID;
};

struct FragmentOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
};

// Box b draws mesh b mod kMeshCount, whose vertices start at kMeshVertexCount * mesh in the combined vertex buffer: the base
// vertex BenchCull.slang writes into the box's record, as Benchmarks/Common/Scene.h defines.
static const uint kMeshCount = 64;
static const uint kMeshVertexCount = 24;

// The box's draw identifier is its scene index; vertex index excludes the base vertex, so the mesh's first vertex is added.
[shader("vertex")]
VertexOutput gbufferVertex(rojoRHI::VertexIds ids)
{
    GeometryRoot root = rojoRHI::root<GeometryRoot>();
    uint box = rojoRHI::drawId(ids);
    Transform instance = rojoRHI::load<Transform>(root.instances, box);
    MeshVertex vertex = rojoRHI::load<MeshVertex>(root.vertices, kMeshVertexCount * (box % kMeshCount) + rojoRHI::vertexIndex(ids));

    float4 local = float4(vertex.position.xyz, 1.0);
    float4 world = float4(dot(instance.row0, local), dot(instance.row1, local), dot(instance.row2, local), 1.0);

    VertexOutput output;
    output.position = float4(dot(root.viewProj0, world), dot(root.viewProj1, world), dot(root.viewProj2, world), dot(root.viewProj3, world));
    output.normal = float3(dot(instance.row0.xyz, vertex.normal.xyz), dot(instance.row1.xyz, vertex.normal.xyz), dot(instance.row2.xyz, vertex.normal.xyz));
    output.drawId = box;
    return output;
}

[shader("fragment")]
FragmentOutput gbufferFragment(VertexOutput input)
{
    float3 color = float3(float(input.drawId % 7) / 7.0, float(input.drawId % 11) / 11.0, float(input.drawId % 13) / 13.0);
    FragmentOutput output;
    output.albedo = float4(0.25 + 0.75 * color, 1.0);
    output.normal = float4(normalize(input.normal), 0.0);
    output.material = float4(float(input.drawId % 4) / 4.0, float(input.drawId % 3) / 3.0, 0.0, 1.0);
    return output;
}
