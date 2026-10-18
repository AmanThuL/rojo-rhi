//----------------------------------------------------------------------------------------------------------------------
/// @file BenchLoadsNative.metal
/// @brief Benchmark kernel G1, native arm: each thread sums 16 uint4 loads through a device pointer held in the root.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>
using namespace metal;

struct LoadsRoot
{
    device const uint4* data;
    device uint4* output;
    uint divergent;
    uint pad;
};

// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the other kernels.
static uint mix(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

kernel void loadsNative(constant LoadsRoot& root [[buffer(0)]], uint lane [[thread_position_in_grid]])
{
    uint key = root.divergent != 0u ? mix(lane) : lane / 32u;
    uint4 sum = uint4(0u);
    for (uint j = 0u; j < 16u; ++j)
    {
        uint index = mix(key * 16u + j) & 65535u;
        sum += root.data[index];
    }
    root.output[lane] = sum;
}
