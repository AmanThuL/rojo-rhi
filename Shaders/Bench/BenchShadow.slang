//----------------------------------------------------------------------------------------------------------------------
/// @file BenchShadow.slang
/// @brief The composed frame's shadow pass: a depth-only vertex entry that draws the culled boxes into one cascade.
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

struct ShadowRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr vertices;
    float4 viewProj0;
    float4 viewProj1;
    float4 viewProj2;
    float4 viewProj3;
};

[shader("vertex")]
float4 shadowVertex(rojoRHI::VertexIds ids) : SV_Position
{
    ShadowRoot root = rojoRHI::root<ShadowRoot>();
    Transform instance = rojoRHI::load<Transform>(root.instances, rojoRHI::drawId(ids));
    MeshVertex vertex = rojoRHI::load<MeshVertex>(root.vertices, rojoRHI::vertexIndex(ids));

    float4 local = float4(vertex.position.xyz, 1.0);
    float4 world = float4(dot(instance.row0, local), dot(instance.row1, local), dot(instance.row2, local), 1.0);
    return float4(dot(root.viewProj0, world), dot(root.viewProj1, world), dot(root.viewProj2, world), dot(root.viewProj3, world));
}
