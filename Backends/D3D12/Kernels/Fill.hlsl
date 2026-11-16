//----------------------------------------------------------------------------------------------------------------------
/// @file Fill.hlsl
/// @brief Defines the D3D12 backend's fill kernel, which writes a 32-bit pattern over words from a root view's address.
//----------------------------------------------------------------------------------------------------------------------
// The build compiles this file with the pinned DXC into signed DXIL and embeds its bytes in the library as
// `rojoRHI::d3d12::kFillKernel` (rule rojorhi.d3d12_kernels). It declares no root signature: the pipeline names the kernel
// root signature, two root constants at b0 and a root unordered-access view at u0, the window's first byte.

/// The root constants of one fill dispatch, laid out as `d3d12::FillConstants` writes them.
struct FillConstants
{
    uint pattern;   ///< The word written, its bytes in little-endian order.
    uint wordCount; ///< How many words the dispatch writes from the root view's address.
};

ConstantBuffer<FillConstants> constants : register(b0);

/// The window the dispatch writes, a root view with no descriptor and no bounds: every write is guarded by `wordCount`.
RWByteAddressBuffer target : register(u0);

/// Threads per group, `d3d12::kFillGroupThreads`.
static const uint kGroupThreads = 64;

/// The most groups one dispatch runs, `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION`.
static const uint kMaxGroups = 65535;

//======================================================================================================================
/// Writes `pattern` to word `i` of the window for every `i < wordCount`, and no word past it. The host dispatches
/// `d3d12::fillGroups(wordCount)` groups, the count computed here, and each thread loops with a stride of the grid's threads
/// over the words past it.
[numthreads(kGroupThreads, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint groups = min((constants.wordCount + kGroupThreads - 1) / kGroupThreads, kMaxGroups);
    const uint stride = groups * kGroupThreads;
    for (uint i = thread.x; i < constants.wordCount; i += stride)
    {
        target.Store(i * 4, constants.pattern);
    }
}
