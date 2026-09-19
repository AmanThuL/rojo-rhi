// GPU oracle for the shadow module: one pass writes depth and one writes the shadow factor.
// Slots match ScenePass: buffers 0/2, shadow texture 3, and comparison sampler 1.
import Shadow;

struct Vertex
{
    float3 position;
};

[[vk::binding(0, 0)]]
StructuredBuffer<Vertex> gVertices : register(t0); // → MSL [[buffer(0)]]

struct PassUniforms
{
    float4x4 lightViewProj;   // world → light clip space, what both passes rasterise with
    float4x4 shadowTransform; // lightViewProj with the NDC → texcoord map baked in
    int filter;               // Shadow.slang's kShadowFilterPcf / kShadowFilterPcss
};

[[vk::binding(2, 0)]]
ConstantBuffer<PassUniforms> gPass : register(b2); // → MSL [[buffer(2)]]

[[vk::binding(3, 0)]]
Texture2D gShadowMap : register(t3); // → MSL [[texture(3)]]

[[vk::binding(1, 1)]]
SamplerComparisonState gShadowSampler : register(s1); // → MSL [[sampler(1)]]

struct VSOutput
{
    float4 position : SV_Position;
    // Keep shadow coordinates homogeneous until fragment interpolation completes.
    float4 shadowPos : POSITION0;
};

[shader("vertex")]
VSOutput vertexMain(uint vid: SV_VertexID)
{
    float4 positionW = float4(gVertices[vid].position, 1.0);
    VSOutput o;
    o.position = mul(gPass.lightViewProj, positionW);
    o.shadowPos = mul(gPass.shadowTransform, positionW);
    return o;
}

[shader("fragment")]
void fragmentDepthOnly()
{
}

[shader("fragment")]
float4 fragmentMain(VSOutput in): SV_Target
{
    float shadow = CalcShadowFactor(in.shadowPos, gShadowMap, gShadowSampler, gPass.filter);
    return float4(shadow, shadow, shadow, 1.0);
}
