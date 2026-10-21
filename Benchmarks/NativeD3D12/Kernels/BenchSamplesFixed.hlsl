//----------------------------------------------------------------------------------------------------------------------
/// @file BenchSamplesFixed.hlsl
/// @brief Benchmark kernel G2, arm F: 16 point samples of 128 textures from a descriptor table, with a static point sampler.
//----------------------------------------------------------------------------------------------------------------------
// The build compiles this file with the pinned DXC into Shaders/BenchSamplesFixed.dxil (rule rojorhi.hlsl). It declares no
// root signature: the pipeline names arm F's, the output's root UAV at u0, one root constant at b0, a descriptor table of
// the 128 SRVs at t0 to t127 and the static sampler at s0. The array index is marked NonUniformResourceIndex in both
// configurations, as arm T's heap index is.

/// One float sum per thread.
RWByteAddressBuffer output : register(u0);

/// The root constant: 1 for the divergent configuration, 0 for the uniform one.
struct FixedConstants
{
    uint divergent;
};

ConstantBuffer<FixedConstants> constants : register(b0);

/// The 128 textures, bound through one descriptor table.
Texture2D<float> textures[128] : register(t0);
/// The root signature's static sampler: point filtering, clamped to the edge.
SamplerState nearest : register(s0);

//======================================================================================================================
/// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the other kernels.
uint mix(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

//======================================================================================================================
/// Sums the thread's 16 point samples, texel (j, 5j mod 16) of the texture its j-th index names, into its float of the output.
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint lane = thread.x;
    const uint key = constants.divergent != 0u ? mix(lane) : lane / 32u;
    float sum = 0.0f;
    for (uint j = 0u; j < 16u; ++j)
    {
        const uint slot = mix(key * 16u + j) & 127u;
        const float2 coordinate = float2(float(j & 15u) + 0.5f, float((j * 5u) & 15u) + 0.5f) / 16.0f;
        sum += textures[NonUniformResourceIndex(slot)].SampleLevel(nearest, coordinate, 0.0f);
    }
    output.Store(lane * 4u, asuint(sum));
}
