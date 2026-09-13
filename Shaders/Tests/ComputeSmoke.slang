// Test-only compute oracle: a storage-buffer fill whose every element is a distinct arithmetic
// result, so a CPU readback asserts exact values rather than "something was written".

struct ComputeParams
{
    uint bias;   // Added to every element, so a run is distinguishable from zeroed memory.
    uint extent; // Square image extent, used by the image oracle sharing this parameter block.
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> gOutput : register(u0); // → MSL [[buffer(0)]]

[[vk::binding(1, 0)]]
ConstantBuffer<ComputeParams> gParams : register(b1); // → MSL [[buffer(1)]]

// One element per thread, so the element count is the dispatch's threadgroup count times 64.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeFillBuffer(uint3 tid: SV_DispatchThreadID)
{
    gOutput[tid.x] = tid.x * 3u + gParams.bias;
}
