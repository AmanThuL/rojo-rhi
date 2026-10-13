// Triangle smoke pipeline pulling vertices from buffer slot 0.
struct Vertex
{
    float2 position;
    float3 color;
};

[[vk::binding(0, 0)]]
StructuredBuffer<Vertex> gVertices : register(t0);  // → MSL [[buffer(0)]] = MTL4ArgumentTable buffer slot 0

struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOutput vertexMain(uint vid: SV_VertexID)
{
    Vertex v = gVertices[vid];
    VSOutput o;
    o.position = float4(v.position, 0.0, 1.0);
    o.color = v.color;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput in): SV_Target
{
    return float4(in.color, 1.0);
}

// A void fragment entry keeps depth-only pipelines free of color outputs.
[shader("fragment")]
void fragmentDepthOnly()
{
}
