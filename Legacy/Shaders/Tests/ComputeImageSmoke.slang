// Test-only compute oracle for storage textures: one kernel writes a gradient, the other reads a
// storage texture and writes its complement, so a stale read shows up as the original gradient
// rather than as noise.
//
// Slang emits every module global as a parameter of every entry point in the module, so both
// texture slots below exist in both kernels and a caller binds both regardless of which it runs.

struct ImageParams
{
    uint extent; // Width and height of the square region the kernels cover.
};

[[vk::binding(0, 0)]]
RWTexture2D<float4> gImage : register(u0); // → MSL [[texture(0)]]

[[vk::binding(1, 0)]]
RWTexture2D<float4> gStorageSource : register(u1); // → MSL [[texture(1)]]

[[vk::binding(2, 0)]]
ConstantBuffer<ImageParams> gParams : register(b1); // → MSL [[buffer(1)]]

// Every channel takes a separate arithmetic path, so a swizzled or half-written texel is visible
// in the readback.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeWriteImage(uint3 tid: SV_DispatchThreadID)
{
    float scale = 1.0 / float(gParams.extent - 1u);
    gImage[tid.xy] = float4(float(tid.x) * scale, float(tid.y) * scale, 0.25, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeInvertImage(uint3 tid: SV_DispatchThreadID)
{
    float4 source = gStorageSource[tid.xy];
    gImage[tid.xy] = float4(1.0 - source.r, 1.0 - source.g, 1.0 - source.b, 1.0);
}
