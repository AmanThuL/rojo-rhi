//----------------------------------------------------------------------------------------------------------------------
/// @file TileShape.h
/// @brief Declares the tile shape of a sparse texture or buffer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The shape of the tiles of a sparse resource and of its mip tail, which `Device::tileShape` returns. Every heap offset and
    /// buffer range of a mapping is a multiple of `bytes`.
    struct TileShape
    {
        /// Texels one tile covers along x; for a buffer, the tile size in bytes.
        uint32_t width = 0;
        /// Texels one tile covers along y; 1 for a buffer.
        uint32_t height = 0;
        /// Texels one tile covers along z; 1 for a buffer.
        uint32_t depth = 0;
        /// Heap bytes one tile maps.
        uint32_t bytes = 0;
        /// First mip level packed into the mip tail; the texture's mip count when it has no tail; zero for a buffer.
        uint32_t mipTailFirst = 0;
        /// Heap bytes the mip tail of one layer needs; zero when there is no tail.
        uint64_t mipTailBytes = 0;
    };
} // namespace rojoRHI
