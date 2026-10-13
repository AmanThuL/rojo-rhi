// Test-only: triangles writing two or three colour attachments in one pass.
struct VSOutput
{
    float4 position : SV_Position;
};

struct FSOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
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

// Both values are exactly representable as halves, so a readback of the second attachment can be
// compared bit for bit rather than within a tolerance.
[shader("fragment")]
FSOutput fragmentMain(VSOutput in)
{
    FSOutput o;
    o.color = float4(1.0, 0.0, 0.0, 1.0);
    o.motion = float2(0.25, -0.5);
    return o;
}

// Three attachments, the third a single-channel reactive-style mask. A separate pair of entry
// points because Metal rejects a pipeline whose fragment writes an attachment the pipeline gives
// no format, so the two-attachment cases above cannot share a fragment stage with this one.
struct FSOutputTriple
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
    float reactive : SV_Target2;
};

// A centred triangle rather than the fullscreen one, so the target keeps texels no fragment
// covers and a readback can tell a written texel from the attachment's own clear.
static const float2 kTriangle[3] = { float2(0.0, 0.5), float2(-0.5, -0.5), float2(0.5, -0.5) };

[shader("vertex")]
VSOutput vertexMainTriple(uint vid: SV_VertexID)
{
    VSOutput o;
    o.position = float4(kTriangle[vid], 0.0, 1.0);
    return o;
}

// 0.75 has no exact eight-bit unorm encoding -- 0.75 * 255 is 191.25 -- so the store rounds and a
// readback reads 191.
[shader("fragment")]
FSOutputTriple fragmentMainTriple(VSOutput in)
{
    FSOutputTriple o;
    o.color = float4(1.0, 0.0, 0.0, 1.0);
    o.motion = float2(0.25, -0.5);
    o.reactive = 0.75;
    return o;
}
