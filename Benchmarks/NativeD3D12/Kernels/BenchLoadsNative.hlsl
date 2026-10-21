//----------------------------------------------------------------------------------------------------------------------
/// @file BenchLoadsNative.hlsl
/// @brief Benchmark kernel G1, native arm: each thread sums 16 uint4 loads from a root SRV, a GPU virtual address.
//----------------------------------------------------------------------------------------------------------------------
// The build compiles this file with the pinned DXC into Shaders/BenchLoadsNative.dxil (rule rojorhi.hlsl). It declares no
// root signature: the pipeline names G1's, the data's root SRV at t0, the output's root UAV at u0 and one root constant at
// b0. The sum and the output layout are BenchLoads.slang's.

/// G1's data, kDataEntries uint4 entries, read with no descriptor fetch.
ByteAddressBuffer data : register(t0);
/// One uint4 sum per thread.
RWByteAddressBuffer output : register(u0);

/// The root constant: 1 for the divergent configuration, 0 for the uniform one.
struct LoadsConstants
{
    uint divergent;
};

ConstantBuffer<LoadsConstants> constants : register(b0);

//======================================================================================================================
/// The hash behind every index; Benchmarks/Common/KernelPatterns.h states it for the CPU and the other kernels.
uint mix(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

//======================================================================================================================
/// Sums the 16 entries of the thread's key into its uint4 of the output.
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint lane = thread.x;
    const uint key = constants.divergent != 0u ? mix(lane) : lane / 32u;
    uint4 sum = uint4(0u, 0u, 0u, 0u);
    for (uint j = 0u; j < 16u; ++j)
    {
        const uint index = mix(key * 16u + j) & 65535u;
        sum += data.Load4(index * 16u);
    }
    output.Store4(lane * 16u, sum);
}
