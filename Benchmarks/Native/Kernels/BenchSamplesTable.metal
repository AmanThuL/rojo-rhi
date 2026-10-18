//----------------------------------------------------------------------------------------------------------------------
/// @file BenchSamplesTable.metal
/// @brief Benchmark kernel G2, arm T: 16 point samples through the view and sampler ID tables, read as the module's lowering reads them.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>
using namespace metal;

struct SamplesRoot
{
    device float* output;
    uint viewBase;
    uint samplerIndex;
    uint divergent;
    uint pad0;
    uint pad1;
    uint pad2;
};

// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the other kernels.
static uint mix(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

kernel void samplesTable(
    constant SamplesRoot& root [[buffer(0)]],
    device uint32_t* viewIds [[buffer(1)]],
    device uint32_t* samplerIds [[buffer(2)]],
    uint lane [[thread_position_in_grid]]
)
{
    uint key = root.divergent != 0u ? mix(lane) : lane / 32u;
    sampler texelSampler = ((device sampler*)samplerIds)[root.samplerIndex];
    float sum = 0.0f;
    for (uint j = 0u; j < 16u; ++j)
    {
        uint texture = mix(key * 16u + j) & 127u;
        float2 coordinate = float2(float(j & 15u) + 0.5f, float((j * 5u) & 15u) + 0.5f) / 16.0f;
        sum += ((device texture2d<float, access::sample>*)viewIds)[root.viewBase + texture].sample(texelSampler, coordinate, level(0.0f)).x;
    }
    root.output[lane] = sum;
}
