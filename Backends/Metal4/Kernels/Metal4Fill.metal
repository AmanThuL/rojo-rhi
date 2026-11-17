//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Fill.metal
/// @brief Defines the fill kernel, which writes a 32-bit pattern over a range of words for a fill whose bytes differ.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    // Matches the layout `metal4::FillParams` writes.
    struct FillParams
    {
        uint pattern; // Its bytes in little-endian order.
        uint words;
    };
    static_assert(sizeof(FillParams) == 8, "FillParams must match metal4::FillParams in Metal4Kernels.h");
} // namespace rojoRHI

kernel void rojorhi_fill(constant rojoRHI::FillParams& p [[buffer(0)]], device uint* dst [[buffer(1)]], uint id [[thread_position_in_grid]])
{
    if (id < p.words)
    {
        dst[id] = p.pattern;
    }
}
