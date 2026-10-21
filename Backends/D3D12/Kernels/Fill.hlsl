//----------------------------------------------------------------------------------------------------------------------
/// @file Fill.hlsl
/// @brief Defines the D3D12 backend's fill kernel, which writes a 32-bit pattern over a range of words of a raw view.
//----------------------------------------------------------------------------------------------------------------------
// The build compiles this file with the pinned DXC into signed DXIL and embeds its bytes in the library as
// `rojoRHI::d3d12::kFillKernel` (rule rojorhi.d3d12_kernels). It declares no root signature: the pipeline names the kernel
// root signature, four root constants at b0 and the view heap indexed directly.

/// The root constants of one fill dispatch, laid out as `d3d12::FillConstants` writes them.
struct FillConstants
{
    uint uavIndex;  ///< The view-heap index of the raw UAV written through.
    uint pattern;   ///< The word written, its bytes in little-endian order.
    uint firstWord; ///< The first word written, counted from the view's first element.
    uint wordCount; ///< How many words the dispatch writes.
};

ConstantBuffer<FillConstants> constants : register(b0);

/// Threads per group, `d3d12::kFillGroupThreads`.
static const uint kGroupThreads = 64;

/// The most groups one dispatch runs, `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION`.
static const uint kMaxGroups = 65535;

//======================================================================================================================
/// Writes `pattern` to words `firstWord + i` for every `i < wordCount`. The host dispatches `d3d12::fillGroups(wordCount)`
/// groups, the count computed here, and each thread loops with a stride of the grid's threads over the words past it.
[numthreads(kGroupThreads, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    RWByteAddressBuffer target = ResourceDescriptorHeap[constants.uavIndex];
    const uint groups = min((constants.wordCount + kGroupThreads - 1) / kGroupThreads, kMaxGroups);
    const uint stride = groups * kGroupThreads;
    for (uint i = thread.x; i < constants.wordCount; i += stride)
    {
        target.Store((constants.firstWord + i) * 4, constants.pattern);
    }
}
