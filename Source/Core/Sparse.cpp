//----------------------------------------------------------------------------------------------------------------------
/// @file Sparse.cpp
/// @brief Implements the tile shapes, the tile mapping state and the checks of the sparse rules of chapter 13.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Sparse.h"

#include "Base/Assert.h"
#include "Core/Textures.h"

#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace rojoRHI::core {
    namespace {
        /// The rule an element of a mapping call breaks, as the last four digits of its identifier, and what is wrong; rule 0 is
        /// no violation.
        struct Violation
        {
            uint32_t rule = 0; ///< The number of the `VU-rojo-sparse-` identifier, or 0.
            std::string text;  ///< What the element does.
        };

        //==============================================================================================================
        [[maybe_unused]] bool isSparse(TextureUsage usage)
        {
            return (usage & TextureUsage::Sparse) != TextureUsage::None;
        }

        //==============================================================================================================
        [[maybe_unused]] bool isSparse(BufferUsage usage)
        {
            return (usage & BufferUsage::Sparse) != BufferUsage::None;
        }

        //==============================================================================================================
        uint64_t ceilDiv(uint64_t value, uint64_t unit)
        {
            return (value + unit - 1) / unit;
        }

        //==============================================================================================================
        /// Calls `visit` with the key of each tile `mapping` names, in the order the reference maps them onto heap tiles: x, then
        /// y, then z, then layer; a mip tail is one tile per layer; a buffer range is one tile per `shape.bytes`.
        template<typename Visit> void forEachTile(const TileMapping& mapping, const TileShape& shape, Visit&& visit)
        {
            if (mapping.buffer.bits != 0)
            {
                const uint64_t first = mapping.bufferOffset / shape.bytes;
                const uint64_t count = ceilDiv(mapping.bufferSize, shape.bytes);
                for (uint64_t index = 0; index < count; ++index)
                {
                    visit(TileKey{ .x = static_cast<uint32_t>(first + index) });
                }
                return;
            }
            const TextureRegion& region = mapping.region;
            if (mapping.mipTail)
            {
                for (uint32_t layer = 0; layer < region.layers; ++layer)
                {
                    visit(TileKey{ .mip = TileKey::kTailMip, .layer = region.layer + layer });
                }
                return;
            }
            const uint32_t countX = static_cast<uint32_t>(ceilDiv(region.extent.width, shape.width));
            const uint32_t countY = static_cast<uint32_t>(ceilDiv(region.extent.height, shape.height));
            const uint32_t countZ = static_cast<uint32_t>(ceilDiv(region.extent.depth, shape.depth));
            for (uint32_t layer = 0; layer < region.layers; ++layer)
            {
                for (uint32_t z = 0; z < countZ; ++z)
                {
                    for (uint32_t y = 0; y < countY; ++y)
                    {
                        for (uint32_t x = 0; x < countX; ++x)
                        {
                            visit(
                                TileKey{ .mip = region.mip,
                                         .layer = region.layer + layer,
                                         .x = region.offset.x / shape.width + x,
                                         .y = region.offset.y / shape.height + y,
                                         .z = region.offset.z / shape.depth + z }
                            );
                        }
                    }
                }
            }
        }

        //==============================================================================================================
        /// Returns the heap bytes the tiles of `mapping` need: a tile each, or `mipTailBytes` for each mip tail.
        [[maybe_unused]] uint64_t heapBytesOf(const TileMapping& mapping, const TileShape& shape)
        {
            uint64_t tiles = 0;
            forEachTile(mapping, shape, [&](const TileKey&) { ++tiles; });
            return tiles * (mapping.mipTail ? shape.mipTailBytes : shape.bytes);
        }

        //==============================================================================================================
        /// Returns the first rule `mapping` breaks, given the records it names: `texture` or `buffer` and `heap`, each null when
        /// the handle is null. Debug builds only; Release reports none.
        [[maybe_unused]] Violation findViolation(
            [[maybe_unused]] const DeviceCore& core,
            [[maybe_unused]] const TileMapping& mapping,
            [[maybe_unused]] const TextureRecord* texture,
            [[maybe_unused]] const BufferRecord* buffer,
            [[maybe_unused]] const HeapRecord* heap
        )
        {
#ifndef NDEBUG
            if ((texture != nullptr) == (buffer != nullptr))
            {
                return { 2, std::format("the mapping names {} textures and buffers; exactly one is required", texture != nullptr ? "both" : "no") };
            }
            const bool sparse = texture != nullptr ? isSparse(texture->usage) : isSparse(buffer->usage);
            if (!sparse)
            {
                return { 3, std::format("\"{}\" was not created with Sparse usage", texture != nullptr ? texture->label : buffer->label) };
            }
            const TileShape shape = texture != nullptr ? textureTileShape(core, *texture) : bufferTileShape();
            const TextureRegion& region = mapping.region;
            if (texture != nullptr && !mapping.mipTail)
            {
                const bool inLevel = region.mip < shape.mipTailFirst;
                const Extent3D level = inLevel ? mipExtent(texture->extent, region.mip) : Extent3D{};
                const auto axisOk = [](uint32_t offset, uint32_t extent, uint32_t tile, uint32_t size) {
                    return extent > 0 && offset % tile == 0 && uint64_t{ offset } + extent <= size && (extent % tile == 0 || offset + extent == size);
                };
                const bool layersOk = region.layers >= 1 && region.layer <= texture->layers && region.layers <= texture->layers - region.layer;
                if (!inLevel || !layersOk || !axisOk(region.offset.x, region.extent.width, shape.width, level.width) ||
                    !axisOk(region.offset.y, region.extent.height, shape.height, level.height) ||
                    !axisOk(region.offset.z, region.extent.depth, shape.depth, level.depth))
                {
                    return { 4,
                             std::format(
                                 "the region of mip {} from ({}, {}, {}) of {}x{}x{} texels over layers {} to {} does not lie in a level below the mip tail at "
                                 "{} on the {}x{}x{} tile grid of \"{}\"",
                                 region.mip,
                                 region.offset.x,
                                 region.offset.y,
                                 region.offset.z,
                                 region.extent.width,
                                 region.extent.height,
                                 region.extent.depth,
                                 region.layer,
                                 region.layer + region.layers,
                                 shape.mipTailFirst,
                                 shape.width,
                                 shape.height,
                                 shape.depth,
                                 texture->label
                             ) };
                }
            }
            if (texture != nullptr && mapping.mipTail)
            {
                if (shape.mipTailFirst >= texture->mips || region.layers < 1 || region.layer > texture->layers ||
                    region.layers > texture->layers - region.layer)
                {
                    return { 5,
                             std::format(
                                 "the mip tail of layers {} to {} is mapped on \"{}\", which has {} layers and {}",
                                 region.layer,
                                 region.layer + region.layers,
                                 texture->label,
                                 texture->layers,
                                 shape.mipTailFirst >= texture->mips ? "no mip tail" : "a mip tail"
                             ) };
                }
            }
            if (buffer != nullptr)
            {
                if (mapping.mipTail)
                {
                    return { 6, std::format("mipTail is set on a mapping of the buffer \"{}\"", buffer->label) };
                }
                const uint64_t offset = mapping.bufferOffset;
                const uint64_t size = mapping.bufferSize;
                if (offset % shape.bytes != 0 || size == 0 || offset > buffer->size || size > buffer->size - offset ||
                    (size % shape.bytes != 0 && offset + size != buffer->size))
                {
                    return { 7,
                             std::format(
                                 "the range of {} bytes from {} of the buffer \"{}\" of {} bytes does not follow the {}-byte tiles",
                                 size,
                                 offset,
                                 buffer->label,
                                 buffer->size,
                                 shape.bytes
                             ) };
                }
            }
            if (mapping.unmap)
            {
                if (heap != nullptr || mapping.heapOffset != 0)
                {
                    return { 10, std::format("an unmap names a heap or a heap offset ({})", mapping.heapOffset) };
                }
                return {};
            }
            if (heap == nullptr || mapping.heapOffset % shape.bytes != 0)
            {
                return { 8, std::format("the heap offset {} of a map is not a multiple of {} or no heap is named", mapping.heapOffset, shape.bytes) };
            }
            const uint64_t bytes = heapBytesOf(mapping, shape);
            if (mapping.heapOffset > heap->size || bytes > heap->size - mapping.heapOffset)
            {
                return {
                    8, std::format("{} bytes of tiles at offset {} do not fit the heap \"{}\" of {} bytes", bytes, mapping.heapOffset, heap->label, heap->size)
                };
            }
            if (heap->memory != Memory::Gpu)
            {
                return { 9, std::format("the heap \"{}\" is not a Memory::Gpu heap", heap->label) };
            }
#endif
            return {};
        }

        //==============================================================================================================
        /// Reports `violation`, the rule an element breaks, with the identifier of that rule.
        [[maybe_unused]] void reportViolation([[maybe_unused]] const Violation& violation)
        {
            ROJORHI_VU(violation.rule != 2, "VU-rojo-sparse-0002", "{}", violation.text);
            ROJORHI_VU(violation.rule != 3, "VU-rojo-sparse-0003", "{}", violation.text);
            ROJORHI_VU(violation.rule != 4, "VU-rojo-sparse-0004", "{}", violation.text);
            ROJORHI_VU(violation.rule != 5, "VU-rojo-sparse-0005", "{}", violation.text);
            ROJORHI_VU(violation.rule != 6, "VU-rojo-sparse-0006", "{}", violation.text);
            ROJORHI_VU(violation.rule != 7, "VU-rojo-sparse-0007", "{}", violation.text);
            ROJORHI_VU(violation.rule != 8, "VU-rojo-sparse-0008", "{}", violation.text);
            ROJORHI_VU(violation.rule != 9, "VU-rojo-sparse-0009", "{}", violation.text);
            ROJORHI_VU(violation.rule != 10, "VU-rojo-sparse-0010", "{}", violation.text);
        }

        //==============================================================================================================
        /// Checks every element of `mappings`, and that no two name the same tile (VU-rojo-sparse-0012). Debug builds only.
        void checkMappings([[maybe_unused]] DeviceCore& core, [[maybe_unused]] std::span<const TileMapping> mappings)
        {
#ifndef NDEBUG
            // This set is linear in the number of tiles the call names; it exists in Debug builds only.
            std::set<std::pair<uint64_t, TileKey>> named;
            for (size_t index = 0; index < mappings.size(); ++index)
            {
                const TileMapping& mapping = mappings[index];
                const TextureRecord* texture = core.pool<Texture>().checked(mapping.texture, true);
                const BufferRecord* buffer = core.pool<Buffer>().checked(mapping.buffer, true);
                const HeapRecord* heap = core.pool<Heap>().checked(mapping.heap, true);
                if (texture != nullptr)
                {
                    checkNotSwapchainImage(*texture, "mapTiles");
                }
                if (const Violation violation = findViolation(core, mapping, texture, buffer, heap); violation.rule != 0)
                {
                    reportViolation(violation);
                }
                const TileShape shape = texture != nullptr ? textureTileShape(core, *texture) : bufferTileShape();
                const uint64_t resource = texture != nullptr ? mapping.texture.bits : mapping.buffer.bits;
                forEachTile(mapping, shape, [&](const TileKey& key) {
                    const bool fresh = named.emplace(resource, key).second;
                    ROJORHI_VU(
                        fresh,
                        "VU-rojo-sparse-0012",
                        "element {} of {} names a tile (mip {}, layer {}, x {}, y {}, z {}) that an earlier element or this one names",
                        index,
                        mappings.size(),
                        key.mip,
                        key.layer,
                        key.x,
                        key.y,
                        key.z
                    );
                });
            }
#endif
        }

        //==============================================================================================================
        /// Adds each delta of `deltas`, keyed by heap handle bits, to the mapped-tile count of its heap.
        void applyHeapDeltas(DeviceCore& core, const std::map<uint64_t, int64_t>& deltas)
        {
            for (const auto& [bits, delta] : deltas)
            {
                if (delta == 0)
                {
                    continue;
                }
                // A heap that is gone cannot hold a mapped tile (VU-rojo-device-0009), so only misuse reaches that case.
                core.pool<Heap>().update(Heap{ bits }, [delta](HeapRecord& record) {
                    ROJORHI_INTERNAL_ASSERT(delta > 0 || record.mappedTiles >= static_cast<uint64_t>(-delta), "a heap counts fewer mapped tiles than it holds");
                    record.mappedTiles += static_cast<uint64_t>(delta);
                });
            }
        }

        //==============================================================================================================
        /// Applies `mapping` to `tiles`, the mapping of the resource it names, and adds the change of each heap's tile count to
        /// `deltas`.
        void applyElement(const TileMapping& mapping, const TileShape& shape, TileMap& tiles, std::map<uint64_t, int64_t>& deltas)
        {
            forEachTile(mapping, shape, [&](const TileKey& key) {
                const auto found = tiles.find(key);
                if (found != tiles.end())
                {
                    --deltas[found->second.bits];
                    if (mapping.unmap)
                    {
                        tiles.erase(found);
                        return;
                    }
                    found->second = mapping.heap;
                    ++deltas[mapping.heap.bits];
                    return;
                }
                if (!mapping.unmap)
                {
                    tiles.emplace(key, mapping.heap);
                    ++deltas[mapping.heap.bits];
                }
            });
        }
    } // namespace

    //==================================================================================================================
    TileShape textureTileShape(const DeviceCore& core, const TextureRecord& record)
    {
        const TileExtent extent = core.tileExtent(record.format, record.type);
        TileShape shape;
        shape.width = extent.width;
        shape.height = extent.height;
        shape.depth = extent.depth;
        shape.bytes = kTileBytes;
        shape.mipTailFirst = record.mips;
        for (uint32_t mip = 0; mip < record.mips; ++mip)
        {
            const Extent3D level = mipExtent(record.extent, mip);
            if (level.width < extent.width || level.height < extent.height || (record.type == TextureType::Tex3D && level.depth < extent.depth))
            {
                shape.mipTailFirst = mip;
                break;
            }
        }
        shape.mipTailBytes = shape.mipTailFirst < record.mips ? kTileBytes : 0;
        return shape;
    }

    //==================================================================================================================
    TileShape bufferTileShape()
    {
        return TileShape{ .width = kTileBytes, .height = 1, .depth = 1, .bytes = kTileBytes, .mipTailFirst = 0, .mipTailBytes = 0 };
    }

    //==================================================================================================================
    TileShape tileShape(DeviceCore& core, Texture texture)
    {
        const TextureRecord* record = core.pool<Texture>().checked(texture, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        checkNotSwapchainImage(*record, "tileShape");
        ROJORHI_VU(isSparse(record->usage), "VU-rojo-sparse-0001", "tileShape is asked of the texture \"{}\", which is not Sparse", record->label);
        return textureTileShape(core, *record);
    }

    //==================================================================================================================
    TileShape tileShape(DeviceCore& core, Buffer buffer)
    {
        [[maybe_unused]] const BufferRecord* record = core.pool<Buffer>().checked(buffer, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        ROJORHI_VU(isSparse(record->usage), "VU-rojo-sparse-0001", "tileShape is asked of the buffer \"{}\", which is not Sparse", record->label);
        return bufferTileShape();
    }

    //==================================================================================================================
    void mapTiles(DeviceCore& core, std::span<const TileMapping> mappings)
    {
        checkMappings(core, mappings);
        std::map<uint64_t, int64_t> deltas;
        for (const TileMapping& mapping : mappings)
        {
            // A handle that no longer names a live resource is misuse, which Release leaves unchecked; in Release a stale handle skips
            // both the state and the count together.
            if (mapping.texture.bits != 0)
            {
                const LookupResult<TextureRecord> found = core.pool<Texture>().lookup(mapping.texture);
                if (found.status == LookupStatus::Ok)
                {
                    const TileShape shape = textureTileShape(core, *found.record);
                    core.pool<Texture>().update(mapping.texture, [&](TextureRecord& record) { applyElement(mapping, shape, record.mapped, deltas); });
                }
            }
            else
            {
                core.pool<Buffer>().update(mapping.buffer, [&](BufferRecord& record) { applyElement(mapping, bufferTileShape(), record.mapped, deltas); });
            }
        }
        // The heap counts change after the resource locks drop; a racing destroy of a heap could slip into that window, and only a
        // misuse of device-0009 reaches it, so the window is accepted.
        applyHeapDeltas(core, deltas);
    }

    //==================================================================================================================
    void releaseTiles(DeviceCore& core, const TileMap& tiles)
    {
        std::map<uint64_t, int64_t> deltas;
        for (const auto& [key, heap] : tiles)
        {
            --deltas[heap.bits];
        }
        applyHeapDeltas(core, deltas);
    }
} // namespace rojoRHI::core
