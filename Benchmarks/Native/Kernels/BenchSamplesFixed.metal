//----------------------------------------------------------------------------------------------------------------------
/// @file BenchSamplesFixed.metal
/// @brief Benchmark kernel G2, arm F: 16 point samples of 128 textures bound in argument-table slots, with a constant nearest sampler.
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

constexpr sampler kNearest(filter::nearest, address::clamp_to_edge, coord::normalized);

// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the other kernels.
static uint mix(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

kernel void samplesFixed(
    constant SamplesRoot& root [[buffer(0)]],
    array<texture2d<float>, 128> textures [[texture(0)]],
    uint lane [[thread_position_in_grid]]
)
{
    uint key = root.divergent != 0u ? mix(lane) : lane / 32u;
    float sum = 0.0f;
    for (uint j = 0u; j < 16u; ++j)
    {
        uint texture = mix(key * 16u + j) & 127u;
        float2 coordinate = float2(float(j & 15u) + 0.5f, float((j * 5u) & 15u) + 0.5f) / 16.0f;
        sum += textures[texture].sample(kNearest, coordinate, level(0.0f)).x;
    }
    root.output[lane] = sum;
}
