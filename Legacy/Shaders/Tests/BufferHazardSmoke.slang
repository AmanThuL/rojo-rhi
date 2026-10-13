// Test-only oracle for buffer hazards: one kernel that reads a storage buffer and writes a
// derived value into another. Whatever produced the source -- a fillBuffer, an earlier dispatch --
// is only visible here if a barrier ordered it, and the bias makes a stale read distinguishable
// from a correct one at every element.

struct HazardParams
{
    uint bias; // Added to every element read, so a run is distinguishable from zeroed memory.
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> gOutput : register(u0); // → MSL [[buffer(0)]]

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> gSource : register(u1); // → MSL [[buffer(1)]]

[[vk::binding(2, 0)]]
ConstantBuffer<HazardParams> gParams : register(b2); // → MSL [[buffer(2)]]

// One element per thread, so the element count is the dispatch's threadgroup count times 64.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeAddFromBuffer(uint3 tid: SV_DispatchThreadID)
{
    gOutput[tid.x] = gSource[tid.x] + gParams.bias;
}
