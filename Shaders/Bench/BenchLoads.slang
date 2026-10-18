//----------------------------------------------------------------------------------------------------------------------
/// @file BenchLoads.slang
/// @brief Benchmark kernel G1: each thread sums 16 uint4 loads through an Addr from the root.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct LoadsRoot
{
    rojoRHI::Addr data;
    rojoRHI::Addr output;
    uint divergent;
    uint pad;
};

// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the native kernels.
uint mix(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void loadsMain(uint3 tid : SV_DispatchThreadID)
{
    LoadsRoot root = rojoRHI::root<LoadsRoot>();
    uint thread = tid.x;
    uint key = root.divergent != 0u ? mix(thread) : thread / 32u;
    uint4 sum = uint4(0u, 0u, 0u, 0u);
    for (uint j = 0u; j < 16u; ++j)
    {
        uint index = mix(key * 16u + j) & 65535u;
        sum += rojoRHI::load<uint4>(root.data, index);
    }
    rojoRHI::store<uint4>(root.output, thread, sum);
}
