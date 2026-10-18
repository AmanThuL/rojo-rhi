//----------------------------------------------------------------------------------------------------------------------
/// @file BenchPost.slang
/// @brief The composed frame's full-screen passes: a triangle vertex entry, bloom down, bloom up and the tone map.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct BloomDownRoot
{
    uint source;
    uint samplerId;
    float texelX;
    float texelY;
};

struct BloomUpRoot
{
    uint source;
    uint samplerId;
    float intensity;
    uint pad0;
};

struct ToneMapRoot
{
    uint source;
    uint samplerId;
    float exposure;
    uint pad0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// One triangle covering the screen from vertex indices 0, 1 and 2.
[shader("vertex")]
VertexOutput fullscreenVertex(rojoRHI::VertexIds ids)
{
    uint index = rojoRHI::vertexIndex(ids);
    float2 corner = float2(float((index << 1) & 2), float(index & 2));
    VertexOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(corner.x, 1.0 - corner.y);
    return output;
}

// Four linear taps half a source texel off the centre.
[shader("fragment")]
float4 bloomDownFragment(VertexOutput input) : SV_Target0
{
    BloomDownRoot root = rojoRHI::root<BloomDownRoot>();
    Texture2D<float4> source = rojoRHI::texture2D<float4>(root.source);
    SamplerState linearSampler = rojoRHI::sampler(root.samplerId);
    float2 offset = 0.5 * float2(root.texelX, root.texelY);
    float4 sum = source.SampleLevel(linearSampler, input.uv + float2(-offset.x, -offset.y), 0)
               + source.SampleLevel(linearSampler, input.uv + float2(offset.x, -offset.y), 0)
               + source.SampleLevel(linearSampler, input.uv + float2(-offset.x, offset.y), 0)
               + source.SampleLevel(linearSampler, input.uv + float2(offset.x, offset.y), 0);
    return 0.25 * sum;
}

// Blended additively into the target by the pipeline.
[shader("fragment")]
float4 bloomUpFragment(VertexOutput input) : SV_Target0
{
    BloomUpRoot root = rojoRHI::root<BloomUpRoot>();
    Texture2D<float4> source = rojoRHI::texture2D<float4>(root.source);
    return root.intensity * source.SampleLevel(rojoRHI::sampler(root.samplerId), input.uv, 0);
}

[shader("fragment")]
float4 toneMapFragment(VertexOutput input) : SV_Target0
{
    ToneMapRoot root = rojoRHI::root<ToneMapRoot>();
    Texture2D<float4> source = rojoRHI::texture2D<float4>(root.source);
    float3 hdr = root.exposure * source.SampleLevel(rojoRHI::sampler(root.samplerId), input.uv, 0).rgb;
    float3 mapped = hdr / (1.0 + hdr);
    return float4(pow(mapped, float3(1.0 / 2.2)), 1.0);
}
