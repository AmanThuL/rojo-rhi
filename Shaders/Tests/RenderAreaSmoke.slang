// Test-only: a fullscreen triangle that paints every fragment it covers red, so a readback tells
// the render area's texels apart from the attachment's clear.
struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vid: SV_VertexID)
{
    // Three-vertex fullscreen triangle: without a render area it covers the whole attachment.
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOutput o;
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOutput in) : SV_Target0
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
