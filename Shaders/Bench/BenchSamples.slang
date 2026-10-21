//----------------------------------------------------------------------------------------------------------------------
/// @file BenchSamples.slang
/// @brief Benchmark kernel G2: each thread sums 16 point samples of R32Float textures named by view IDs from the root.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct SamplesRoot
{
    rojoRHI::Addr output;
    uint viewBase;
    uint samplerIndex;
    uint divergent;
    uint pad0;
    uint pad1;
    uint pad2;
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
void samplesMain(uint3 tid : SV_DispatchThreadID)
{
    SamplesRoot root = rojoRHI::root<SamplesRoot>();
    uint thread = tid.x;
    uint key = root.divergent != 0u ? mix(thread) : thread / 32u;
    SamplerState texelSampler = rojoRHI::sampler(root.samplerIndex);
    float sum = 0.0;
    for (uint j = 0u; j < 16u; ++j)
    {
        uint texture = mix(key * 16u + j) & 127u;
        Texture2D<float> source = rojoRHI::texture2D<float>(rojoRHI::nonUniform(root.viewBase + texture));
        float2 coordinate = float2(float(j & 15u) + 0.5, float((j * 5u) & 15u) + 0.5) / 16.0;
        sum += source.SampleLevel(texelSampler, coordinate, 0.0);
    }
    rojoRHI::store<float>(root.output, thread, sum);
}
