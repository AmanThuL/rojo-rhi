import RojoRHI;

static const uint kSide = 24;

struct Mat4
{
    float4 r0;
    float4 r1;
    float4 r2;
    float4 r3;
};

struct SceneRoot
{
    Mat4 viewProj;
    Mat4 previousViewProj;
    float2 jitter;
    float time;
    float previousTime;
    float2 inputSize;
    uint pad0;
    uint pad1;
};

struct PresentRoot
{
    uint image;
    uint sampler;
};

struct SceneVaryings
{
    float4 position : SV_Position;
    float4 current : TEXCOORD0;
    float4 previous : TEXCOORD1;
    float3 local : TEXCOORD2;
    float3 normal : NORMAL;
    nointerpolation float phase : TEXCOORD3;
};

struct SceneTargets
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

struct PresentVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
static const float3 kNormals[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };

float4 transform(Mat4 m, float4 p)
{
    return float4(dot(m.r0, p), dot(m.r1, p), dot(m.r2, p), dot(m.r3, p));
}

float3 rotate(float3 p, float yaw, float pitch)
{
    float cy = cos(yaw);
    float sy = sin(yaw);
    p = float3(cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z);
    float cp = cos(pitch);
    float sp = sin(pitch);
    return float3(p.x, cp * p.y - sp * p.z, sp * p.y + cp * p.z);
}

float3 place(uint instance, float phase, float3 p, float t)
{
    float2 cell = float2(instance % kSide, instance / kSide) - 0.5 * float(kSide - 1);
    float3 center = float3(1.2 * cell.x, 0.6 * sin(0.8 * t + 6.28 * phase), 1.2 * cell.y);
    return center + rotate(0.45 * p, t * (0.6 + phase), t * (0.4 + 0.5 * phase));
}

[shader("vertex")]
SceneVaryings sceneVertex(rojoRHI::VertexIds ids)
{
    SceneRoot root = rojoRHI::root<SceneRoot>();
    uint instance = rojoRHI::instanceIndex(ids);
    uint vertex = rojoRHI::vertexIndex(ids);
    float phase = frac(sin(float(instance) * 12.9898) * 43758.5453);

    float3 n = kNormals[vertex / 6];
    float3 u = n.yzx;
    float3 v = cross(n, u);
    float2 c = kCorners[vertex % 6];
    float3 local = 0.5 * (n + c.x * u + c.y * v);

    SceneVaryings output;
    output.current = transform(root.viewProj, float4(place(instance, phase, local, root.time), 1.0));
    output.previous = transform(root.previousViewProj, float4(place(instance, phase, local, root.previousTime), 1.0));
    output.position = output.current + float4(root.jitter * output.current.w, 0.0, 0.0);
    output.local = local;
    output.normal = rotate(n, root.time * (0.6 + phase), root.time * (0.4 + 0.5 * phase));
    output.phase = phase;
    return output;
}

[shader("fragment")]
SceneTargets sceneFragment(SceneVaryings input)
{
    SceneRoot root = rojoRHI::root<SceneRoot>();

    float3 cell = floor(input.local * 8.0 + 0.001);
    float checker = fmod(abs(cell.x + cell.y + cell.z), 2.0);
    float hue = 6.2831853 * input.phase;
    float3 albedo = 0.5 + 0.5 * cos(float3(hue, hue - 2.1, hue + 2.1));
    albedo *= lerp(0.25, 1.0, checker);

    float light = 0.15 + saturate(dot(normalize(input.normal), normalize(float3(0.4, 1.0, 0.3))));
    float glow = input.phase > 0.92 ? 6.0 : 0.0;

    float2 current = input.current.xy / input.current.w;
    float2 previous = input.previous.xy / input.previous.w;

    SceneTargets output;
    output.color = float4(albedo * (light + glow), 1.0);
    output.motion = (previous - current) * float2(0.5, -0.5) * root.inputSize;
    return output;
}

[shader("vertex")]
PresentVaryings presentVertex(rojoRHI::VertexIds ids)
{
    uint vertex = rojoRHI::vertexIndex(ids);
    float2 uv = float2(float((vertex << 1) & 2), float(vertex & 2));

    PresentVaryings output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 encodeSrgb(float3 linear)
{
    return select(linear <= 0.0031308, linear * 12.92, 1.055 * pow(linear, 1.0 / 2.4) - 0.055);
}

[shader("fragment")]
float4 presentFragment(PresentVaryings input) : SV_Target
{
    PresentRoot root = rojoRHI::root<PresentRoot>();
    float3 hdr = rojoRHI::texture2D(root.image).Sample(rojoRHI::sampler(root.sampler), input.uv).rgb;
    return float4(encodeSrgb(hdr / (1.0 + hdr)), 1.0);
}
