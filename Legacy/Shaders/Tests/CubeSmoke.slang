// Cubemap smoke oracle: every fragment samples the center of +X from texture slot 2.
// Texture, buffer, and sampler slots occupy separate Metal argument-table index spaces.
[[vk::binding(0, 0)]]
TextureCube<float4> gCube : register(t2);  // → MSL [[texture(2)]]

[[vk::binding(0, 1)]]
SamplerState gSampler : register(s0);      // → MSL [[sampler(0)]]

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vid: SV_VertexID)
{
    // Three-vertex fullscreen triangle.
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOutput o;
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput in): SV_Target
{
    // The +X face center avoids cross-face filtering.
    return gCube.Sample(gSampler, float3(1.0, 0.0, 0.0));
}
