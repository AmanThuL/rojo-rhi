import RojoRHI;

struct Particle
{
    float2 position;
    float2 velocity;
};

struct SimulateRoot
{
    rojoRHI::Addr source;
    rojoRHI::Addr target;
    float time;
    float dt;
    uint count;
    uint pad;
};

struct DrawRoot
{
    rojoRHI::Addr particles;
    float aspect;
    float size;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    float speed : TEXCOORD1;
};

float2 pull(float2 position, float2 attractor)
{
    float2 d = attractor - position;
    return d * rsqrt(dot(d, d) + 0.02) / (dot(d, d) + 0.05);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void simulate(uint3 thread : SV_DispatchThreadID)
{
    SimulateRoot root = rojoRHI::root<SimulateRoot>();
    if (thread.x >= root.count)
    {
        return;
    }

    Particle particle = rojoRHI::load<Particle>(root.source, thread.x);
    float2 a = 0.6 * float2(cos(0.7 * root.time), sin(1.1 * root.time));
    float2 b = 0.5 * float2(sin(0.9 * root.time + 1.0), cos(0.5 * root.time));

    float2 force = 0.08 * (pull(particle.position, a) + pull(particle.position, b));
    particle.velocity = (particle.velocity + force * root.dt) * 0.995;
    particle.position += particle.velocity * root.dt;
    rojoRHI::store<Particle>(root.target, thread.x, particle);
}

static const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };

[shader("vertex")]
Varyings particleVertex(rojoRHI::VertexIds ids)
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    Particle particle = rojoRHI::load<Particle>(root.particles, rojoRHI::instanceIndex(ids));
    float2 corner = kCorners[rojoRHI::vertexIndex(ids)];

    Varyings output;
    output.position = float4((particle.position.x + corner.x * root.size) / root.aspect, particle.position.y + corner.y * root.size, 0.0, 1.0);
    output.local = corner;
    output.speed = length(particle.velocity);
    return output;
}

[shader("fragment")]
float4 particleFragment(Varyings input) : SV_Target
{
    float falloff = saturate(1.0 - dot(input.local, input.local));
    float3 color = lerp(float3(0.1, 0.3, 1.0), float3(1.0, 0.5, 0.1), saturate(input.speed * 0.8));
    return float4(color * falloff * 0.35, 1.0);
}
