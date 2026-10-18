//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Fill.metal
/// @brief Defines the fill kernel, which writes a 32-bit pattern over a range of words for a fill whose bytes differ.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    /// The parameters of one fill dispatch, laid out as `metal4::FillParams` writes them.
    struct FillParams
    {
        uint pattern; ///< The word written, its bytes in little-endian order.
        uint words;   ///< How many words the dispatch writes.
    };
    static_assert(sizeof(FillParams) == 8, "FillParams must match metal4::FillParams in Metal4Kernels.h");
} // namespace rojoRHI

//======================================================================================================================
/// Writes `p.pattern` to word `id` of `dst`, one thread per word; threads past `p.words` write nothing.
kernel void rojorhi_fill(constant rojoRHI::FillParams& p [[buffer(0)]], device uint* dst [[buffer(1)]], uint id [[thread_position_in_grid]])
{
    if (id < p.words)
    {
        dst[id] = p.pattern;
    }
}
