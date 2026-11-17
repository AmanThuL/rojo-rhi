//----------------------------------------------------------------------------------------------------------------------
/// @file Fill.hlsl
/// @brief Defines the D3D12 backend's fill kernel, which writes a 32-bit pattern over words from a root view's address.
//----------------------------------------------------------------------------------------------------------------------
// Compiled by the pinned DXC into signed DXIL and embedded as `rojoRHI::d3d12::kFillKernel`. It declares no root
// signature: the pipeline names the kernel root signature, two root constants at b0 and a root UAV at u0, the window's
// first byte.

// Laid out as `d3d12::FillConstants` writes them.
struct FillConstants
{
    uint pattern; // Its bytes in little-endian order.
    uint wordCount;
};

ConstantBuffer<FillConstants> constants : register(b0);

// A root view has no descriptor and no bounds: every write is guarded by `wordCount`.
RWByteAddressBuffer target : register(u0);

// Equals `d3d12::kFillGroupThreads`.
static const uint kGroupThreads = 64;

// D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION.
static const uint kMaxGroups = 65535;

// The host dispatches `d3d12::fillGroups(wordCount)` groups, the count computed here; each thread strides by the grid's
// thread count, so no word past `wordCount` is written.
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
