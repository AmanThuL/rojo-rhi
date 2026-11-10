import RojoRHI;

struct Instance
{
    float2 offset;
    float scale;
    uint texture;
};

struct BindlessRoot
{
    rojoRHI::Addr instances;
    uint sampler;
    float time;
    float aspect;
    uint pad;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint texture : TEXCOORD1;
};

static const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };

[shader("vertex")]
Varyings bindlessVertex(rojoRHI::VertexIds ids)
{
    BindlessRoot root = rojoRHI::root<BindlessRoot>();
    Instance instance = rojoRHI::load<Instance>(root.instances, rojoRHI::instanceIndex(ids));
    float2 corner = kCorners[rojoRHI::vertexIndex(ids)];

    float wobble = 1.0 + 0.2 * sin(2.0 * root.time + 3.0 * instance.offset.x + 5.0 * instance.offset.y);
    float2 position = instance.offset + corner * instance.scale * wobble;

    Varyings output;
    output.position = float4(position.x / root.aspect, position.y, 0.0, 1.0);
    output.uv = corner * float2(0.5, -0.5) + 0.5;
    output.texture = instance.texture;
    return output;
}

[shader("fragment")]
float4 bindlessFragment(Varyings input) : SV_Target
{
    BindlessRoot root = rojoRHI::root<BindlessRoot>();
    return rojoRHI::texture2D(rojoRHI::nonUniform(input.texture)).Sample(rojoRHI::sampler(root.sampler), input.uv);
}
