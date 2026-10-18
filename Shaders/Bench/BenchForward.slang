//----------------------------------------------------------------------------------------------------------------------
/// @file BenchForward.slang
/// @brief The composed frame's forward pass: one alpha-blended box per draw, its transform and color in the root.
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

struct ForwardRoot
{
    float4 transform0;
    float4 transform1;
    float4 transform2;
    float4 transform3;
    float4 color;
    rojoRHI::Addr vertices;
    rojoRHI::Addr camera;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
};

[shader("vertex")]
VertexOutput forwardVertex(rojoRHI::VertexIds ids)
{
    ForwardRoot root = rojoRHI::root<ForwardRoot>();
    MeshVertex vertex = rojoRHI::load<MeshVertex>(root.vertices, rojoRHI::vertexIndex(ids));

    float4 local = float4(vertex.position.xyz, 1.0);
    float4 world = float4(dot(root.transform0, local), dot(root.transform1, local), dot(root.transform2, local), 1.0);

    Transform camera = rojoRHI::load<Transform>(root.camera, 0);

    VertexOutput output;
    output.position = float4(dot(camera.row0, world), dot(camera.row1, world), dot(camera.row2, world), dot(camera.row3, world));
    output.normal = float3(dot(root.transform0.xyz, vertex.normal.xyz), dot(root.transform1.xyz, vertex.normal.xyz), dot(root.transform2.xyz, vertex.normal.xyz));
    return output;
}

[shader("fragment")]
float4 forwardFragment(VertexOutput input) : SV_Target0
{
    ForwardRoot root = rojoRHI::root<ForwardRoot>();
    float shade = 0.4 + 0.6 * saturate(dot(normalize(input.normal), normalize(float3(0.4, 1.0, 0.3))));
    return float4(root.color.rgb * shade, root.color.a);
}
