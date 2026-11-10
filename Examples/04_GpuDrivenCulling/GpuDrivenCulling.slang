import RojoRHI;

struct Mat4
{
    float4 r0;
    float4 r1;
    float4 r2;
    float4 r3;
};

struct Instance
{
    float4 positionScale;
    float4 color;
};

struct MeshVertex
{
    float4 position;
    float4 normal;
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

struct CullRoot
{
    Mat4 viewProj;
    rojoRHI::Addr instances;
    rojoRHI::Addr records;
    rojoRHI::Addr count;
    uint instanceCount;
    uint indexCount;
};

struct DrawRoot
{
    Mat4 viewProj;
    rojoRHI::Addr instances;
    rojoRHI::Addr vertices;
};

struct Varyings
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR0;
};

float4 transform(Mat4 m, float4 p)
{
    return float4(dot(m.r0, p), dot(m.r1, p), dot(m.r2, p), dot(m.r3, p));
}

bool outside(float4 plane, float3 center, float radius)
{
    return dot(plane.xyz, center) + plane.w < -radius * length(plane.xyz);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull(uint3 thread : SV_DispatchThreadID)
{
    CullRoot root = rojoRHI::root<CullRoot>();
    if (thread.x >= root.instanceCount)
    {
        return;
    }

    Instance instance = rojoRHI::load<Instance>(root.instances, thread.x);
    float3 center = instance.positionScale.xyz;
    float radius = 0.87 * instance.positionScale.w;

    Mat4 m = root.viewProj;
    if (outside(m.r3 + m.r0, center, radius) || outside(m.r3 - m.r0, center, radius) ||
        outside(m.r3 + m.r1, center, radius) || outside(m.r3 - m.r1, center, radius) ||
        outside(m.r3 - m.r2, center, radius))
    {
        return;
    }

    DrawIndexedIndirectArgs record;
    record.drawId = thread.x;
    record.indexCount = root.indexCount;
    record.instanceCount = 1;
    record.firstIndex = 0;
    record.baseVertex = 0;
    record.firstInstance = 0;

    uint slot = rojoRHI::atomicAdd(root.count, 0, 1);
    rojoRHI::store<DrawIndexedIndirectArgs>(root.records, slot, record);
}

[shader("vertex")]
Varyings sceneVertex(rojoRHI::VertexIds ids)
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    Instance instance = rojoRHI::load<Instance>(root.instances, rojoRHI::drawId(ids));
    MeshVertex vertex = rojoRHI::load<MeshVertex>(root.vertices, rojoRHI::vertexIndex(ids));

    float3 world = vertex.position.xyz * instance.positionScale.w + instance.positionScale.xyz;

    Varyings output;
    output.position = transform(root.viewProj, float4(world, 1.0));
    output.normal = vertex.normal.xyz;
    output.color = instance.color.rgb;
    return output;
}

[shader("fragment")]
float4 sceneFragment(Varyings input) : SV_Target
{
    float light = 0.25 + 0.75 * saturate(dot(normalize(input.normal), normalize(float3(0.4, 1.0, 0.3))));
    return float4(input.color * light, 1.0);
}
