//----------------------------------------------------------------------------------------------------------------------
/// @file BenchLighting.slang
/// @brief The composed frame's lighting kernel: shades the G-buffer with one directional light, three cascades and ao.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct LightingRoot
{
    uint albedo;
    uint normal;
    uint material;
    uint depth;
    uint shadow;
    uint ao;
    uint hdr;
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 lightDirection;
    float4 inverseViewProj0;
    float4 inverseViewProj1;
    float4 inverseViewProj2;
    float4 inverseViewProj3;
    float4 cascade0Row0;
    float4 cascade0Row1;
    float4 cascade0Row2;
    float4 cascade0Row3;
    float4 cascade1Row0;
    float4 cascade1Row1;
    float4 cascade1Row2;
    float4 cascade1Row3;
    float4 cascade2Row0;
    float4 cascade2Row1;
    float4 cascade2Row2;
    float4 cascade2Row3;
};

static const float kShadowSize = 2048.0;
static const float kShadowBias = 0.0005;

float4 project(float4 row0, float4 row1, float4 row2, float4 row3, float4 point)
{
    return float4(dot(row0, point), dot(row1, point), dot(row2, point), dot(row3, point));
}

// 1 when the world position is lit by cascade `layer`, 0 when shadowed, and -1 when it lies outside the cascade.
// Reverse Z: an occluder nearer the light stores a larger depth than the receiver.
float cascadeLight(Texture2DArray<float> shadow, float4 row0, float4 row1, float4 row2, float4 row3, float4 world, int layer)
{
    float4 clip = project(row0, row1, row2, row3, world);
    float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0 || clip.z < 0.0 || clip.z > 1.0)
    {
        return -1.0;
    }
    int2 texel = int2(uv * kShadowSize);
    float occluder = shadow.Load(int4(texel, layer, 0));
    return occluder > clip.z + kShadowBias ? 0.0 : 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void lightingMain(uint3 tid : SV_DispatchThreadID)
{
    LightingRoot root = rojoRHI::root<LightingRoot>();
    if (tid.x >= root.width || tid.y >= root.height)
    {
        return;
    }
    Texture2D<float4> albedoTexture = rojoRHI::texture2D<float4>(root.albedo);
    Texture2D<float4> normalTexture = rojoRHI::texture2D<float4>(root.normal);
    Texture2D<float4> materialTexture = rojoRHI::texture2D<float4>(root.material);
    Texture2D<float> depthTexture = rojoRHI::texture2D<float>(root.depth);
    Texture2DArray<float> shadowTexture = rojoRHI::texture2DArray<float>(root.shadow);
    Texture2D<float> aoTexture = rojoRHI::texture2D<float>(root.ao);
    RWTexture2D<float4> output = rojoRHI::rwTexture2D<float4>(root.hdr);

    int3 texel = int3(int2(tid.xy), 0);
    float depth = depthTexture.Load(texel);
    if (depth <= 0.0)
    {
        output[tid.xy] = float4(0.05, 0.07, 0.10, 1.0);
        return;
    }

    float2 ndc = float2((float(tid.x) + 0.5) / float(root.width) * 2.0 - 1.0, 1.0 - (float(tid.y) + 0.5) / float(root.height) * 2.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 world = project(root.inverseViewProj0, root.inverseViewProj1, root.inverseViewProj2, root.inverseViewProj3, clip);
    world = float4(world.xyz / world.w, 1.0);

    float3 albedo = albedoTexture.Load(texel).rgb;
    float3 normal = normalize(normalTexture.Load(texel).xyz);
    float4 material = materialTexture.Load(texel);
    float occlusion = aoTexture.Load(texel);

    float light = cascadeLight(shadowTexture, root.cascade0Row0, root.cascade0Row1, root.cascade0Row2, root.cascade0Row3, world, 0);
    if (light < 0.0)
    {
        light = cascadeLight(shadowTexture, root.cascade1Row0, root.cascade1Row1, root.cascade1Row2, root.cascade1Row3, world, 1);
    }
    if (light < 0.0)
    {
        light = cascadeLight(shadowTexture, root.cascade2Row0, root.cascade2Row1, root.cascade2Row2, root.cascade2Row3, world, 2);
    }
    light = max(light, 0.0);

    float3 toLight = -normalize(root.lightDirection.xyz);
    float diffuse = saturate(dot(normal, toLight));
    float3 ambient = float3(0.12, 0.14, 0.18) * occlusion;
    float3 color = albedo * (ambient + diffuse * light * (1.0 - 0.5 * material.g) * 1.6);
    output[tid.xy] = float4(color, 1.0);
}
