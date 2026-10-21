//----------------------------------------------------------------------------------------------------------------------
/// @file BenchSamplesTable.hlsl
/// @brief Benchmark kernel G2, arm T: 16 point samples through ResourceDescriptorHeap and SamplerDescriptorHeap indices.
//----------------------------------------------------------------------------------------------------------------------
// The build compiles this file with the pinned DXC into Shaders/BenchSamplesTable.dxil (rule rojorhi.hlsl). It declares no
// root signature: the pipeline names arm T's, the output's root UAV at u0, three root constants at b0 and both heaps
// indexed directly. The fetches are those the module lowers BenchSamples.slang's to: the texture's heap index marked
// NonUniformResourceIndex in both configurations, as that shader marks it with nonUniform, and a plain sampler index.

/// One float sum per thread.
RWByteAddressBuffer output : register(u0);

/// The root constants: the heap index of texture 0 (the 128 views are consecutive), the sampler's heap index, and 1 for
/// the divergent configuration.
struct SamplesConstants
{
    uint viewBase;
    uint samplerIndex;
    uint divergent;
};

ConstantBuffer<SamplesConstants> constants : register(b0);

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
    SamplerState texelSampler = SamplerDescriptorHeap[constants.samplerIndex];
    float sum = 0.0f;
    for (uint j = 0u; j < 16u; ++j)
    {
        const uint slot = mix(key * 16u + j) & 127u;
        Texture2D<float> source = ResourceDescriptorHeap[NonUniformResourceIndex(constants.viewBase + slot)];
        const float2 coordinate = float2(float(j & 15u) + 0.5f, float((j * 5u) & 15u) + 0.5f) / 16.0f;
        sum += source.SampleLevel(texelSampler, coordinate, 0.0f);
    }
    output.Store(lane * 4u, asuint(sum));
}
