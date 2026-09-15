// Pins the Metal lowering of the instance index used by production indirect draws.
[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> gObserved : register(u0);

[shader("vertex")]
float4 vertexMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) : SV_Position
{
    if (vertexID == 0u && instanceID < 16u)
        gObserved[instanceID] = instanceID;
    float2 positions[3] = {float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5)};
    return float4(positions[vertexID], 0.5, 1.0);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target { return float4(1.0); }
