//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Sparse.h
/// @brief Declares the Metal 4 placement sparse helpers: format support, measured tile shapes and the native tile mappings.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace rojoRHI {
    struct QueueImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    /// Returns the texels one 64 KiB sparse tile of a `type` texture of `format` covers on `device`, one sample per texel; a zero
    /// width means the device has no sparse tile for that type and format.
    MTL::Size sparseTileExtent(MTL::Device* device, MTL::TextureType type, MTL::PixelFormat format);

    /// Returns true when `device` supports placement sparse resources and has a 64 KiB tile for a 2D texture of `format`, the
    /// probe behind `FormatCapabilities::sparse`.
    bool sparseFormat(MTL::Device* device, MTL::PixelFormat format);

    /// Returns the tile shape of `texture`, a sparse texture of `mips` levels created on `device` with 64 KiB pages: the tile
    /// extent of its type and format, 64 KiB tiles, and its own tail properties. A `firstMipmapInTail` at or past `mips` means no
    /// tail, so `mipTailFirst` is then `mips` and `mipTailBytes` zero, whatever `tailSizeInBytes` reports.
    TileShape measureTiles(MTL::Device* device, const MTL::Texture* texture, uint32_t mips);

    /// What one element of a mapping call names natively: its texture or buffer, the heap it maps from (null for an unmap) and
    /// the resource's tile shape. Both resources are null when the element's handle names no live resource, which only an
    /// unchecked misuse in Release reaches; such an element is skipped.
    struct MappingTarget
    {
        const MTL::Texture* texture = nullptr; ///< The sparse texture, or null.
        const MTL::Buffer* buffer = nullptr;   ///< The sparse buffer, or null.
        const MTL::Heap* heap = nullptr;       ///< The heap that supplies the tiles; null for an unmap.
        TileShape shape = {};                  ///< The tile shape of the texture or buffer.
    };

    /// The operations of one native update call: every element of a mapping call that names the same resource, heap and mode.
    struct MappingGroup
    {
        const MTL::Texture* texture = nullptr;                                    ///< The texture the call updates, or null.
        const MTL::Buffer* buffer = nullptr;                                      ///< The buffer the call updates, or null.
        const MTL::Heap* heap = nullptr;                                          ///< The heap the call maps from; null for an unmap.
        bool unmap = false;                                                       ///< True for an unmap group.
        std::vector<MTL4::UpdateSparseTextureMappingOperation> textureOperations; ///< The operations of a texture group.
        std::vector<MTL4::UpdateSparseBufferMappingOperation> bufferOperations;   ///< The operations of a buffer group.
    };

    /// Returns what each element of `mappings` names natively, in order, reading the pools of `core`: each texture's measured
    /// tile shape and each resource's and heap's native object. It changes nothing.
    std::vector<MappingTarget> resolveMappings(core::DeviceCore& core, std::span<const TileMapping> mappings);

    /// Returns the native update calls that apply `mappings`, whose elements `targets` resolved, without issuing them: one group
    /// per resource, heap and mode, the texture groups before the buffer groups, each in order of first appearance. A texture region is one operation per
    /// layer, its region and heap offset counted in tiles, each layer's heap offset following the previous layer's tiles; a mip tail is one operation per layer
    /// naming the first tail level, with a region of its tail's tiles along x, each layer's heap offset following the previous layer's tail; a buffer range is
    /// one operation, its range and heap offset counted in tiles. An unmap has no heap and a heap offset of zero. Grouping and reordering keep the effect of
    /// the call because no two elements name the same tile (VU-rojo-sparse-0012).
    std::vector<MappingGroup> groupMappings(std::span<const TileMapping> mappings, std::span<const MappingTarget> targets);

    /// Applies `mappings` on `queue`: the core checks and records them, then each group of `groupMappings` is one
    /// `updateTextureMappings` or `updateBufferMappings` on the native queue, and a call that maps a buffer ends with a signal
    /// of the queue's progress event at its current value, which keeps a later texture mapping on the queue from faulting in
    /// Metal's driver. Each native call counts in `nativeSubmitCalls`; an empty span issues nothing. The caller serialises the
    /// call with the queue's submissions.
    void mapTiles(QueueImpl& queue, std::span<const TileMapping> mappings);
} // namespace rojoRHI::metal4
