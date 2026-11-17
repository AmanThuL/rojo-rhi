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
    // One sample per texel; a zero width means the device has no sparse tile for that type and format.
    MTL::Size sparseTileExtent(MTL::Device* device, MTL::TextureType type, MTL::PixelFormat format);

    // The probe behind `FormatCapabilities::sparse`.
    bool sparseFormat(MTL::Device* device, MTL::PixelFormat format);

    // A `firstMipmapInTail` at or past `mips` means no tail: `mipTailFirst` is then `mips` and `mipTailBytes` zero,
    // whatever `tailSizeInBytes` reports.
    TileShape measureTiles(MTL::Device* device, const MTL::Texture* texture, uint32_t mips);

    // Both resources are null when the element's handle names no live resource, which only an unchecked misuse in
    // Release reaches; such an element is skipped.
    struct MappingTarget
    {
        const MTL::Texture* texture = nullptr;
        const MTL::Buffer* buffer = nullptr;
        const MTL::Heap* heap = nullptr; // Null for an unmap.
        TileShape shape = {};
    };

    // Every element of a mapping call that names the same resource, heap and mode.
    struct MappingGroup
    {
        const MTL::Texture* texture = nullptr;
        const MTL::Buffer* buffer = nullptr;
        const MTL::Heap* heap = nullptr; // Null for an unmap.
        bool unmap = false;
        std::vector<MTL4::UpdateSparseTextureMappingOperation> textureOperations;
        std::vector<MTL4::UpdateSparseBufferMappingOperation> bufferOperations;
    };

    std::vector<MappingTarget> resolveMappings(core::DeviceCore& core, std::span<const TileMapping> mappings);

    // Texture groups come before buffer groups, each in order of first appearance. Grouping and reordering keep the
    // call's effect because no two elements name the same tile (VU-rojo-sparse-0012).
    std::vector<MappingGroup> groupMappings(std::span<const TileMapping> mappings, std::span<const MappingTarget> targets);

    // Each group is one native update between two signal-and-wait pairs on the queue's progress event: the first holds
    // the updates until earlier work completes, the second holds later work until they complete. The caller serialises
    // the call with the queue's submissions.
    void mapTiles(QueueImpl& queue, std::span<const TileMapping> mappings);
} // namespace rojoRHI::metal4
