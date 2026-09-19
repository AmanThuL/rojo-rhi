// Test-only: fullscreen triangle that copies a texture via Load — no sampler state needed.
[[vk::binding(0, 0)]]
Texture2D<float4> gSource : register(t0);  // → MSL [[texture(0)]]

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
    return gSource.Load(int3(int2(in.position.xy), 0));
}
