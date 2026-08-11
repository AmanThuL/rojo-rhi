// Test-only oracle for indirect arguments produced on the GPU: the kernels below write the raw
// 32-bit words of DispatchIndirectArgs and DrawIndexedIndirectArgs into a storage buffer, in the
// order RHI.h documents. Nothing checks that order but the conformance tests that consume the
// result, which is exactly why the words are written one index at a time here rather than through a
// mirrored struct that could drift in step with a mistake.

struct IndirectParams
{
    uint argsIndex;    // Element index of the first argument word, so the tests can use an offset.
    uint threadgroupsX;// Threadgroup count written into the dispatch arguments.
    uint indexCount;   // Index count written into the indexed-draw arguments.
    uint firstIndex;   // Element offset into the index buffer.
    uint baseVertex;   // Added to every index before the vertex is fetched.
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> gArgs : register(u0); // → MSL [[buffer(0)]]

[[vk::binding(1, 0)]]
ConstantBuffer<IndirectParams> gParams : register(b1); // → MSL [[buffer(1)]]

// One thread: the arguments are a handful of words, and a wider dispatch would only race itself.
[shader("compute")]
[numthreads(1, 1, 1)]
void computeWriteDispatchArgs(uint3 tid: SV_DispatchThreadID)
{
    gArgs[gParams.argsIndex + 0u] = gParams.threadgroupsX;
    gArgs[gParams.argsIndex + 1u] = 1u;
    gArgs[gParams.argsIndex + 2u] = 1u;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void computeWriteDrawIndexedArgs(uint3 tid: SV_DispatchThreadID)
{
    gArgs[gParams.argsIndex + 0u] = gParams.indexCount;
    gArgs[gParams.argsIndex + 1u] = 1u; // instanceCount
    gArgs[gParams.argsIndex + 2u] = gParams.firstIndex;
    gArgs[gParams.argsIndex + 3u] = gParams.baseVertex;
    gArgs[gParams.argsIndex + 4u] = 0u; // firstInstance
}
