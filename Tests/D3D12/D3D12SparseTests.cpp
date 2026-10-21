//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12SparseTests.cpp
/// @brief Tests D3D12 reserved resources: the tiled-resources tier, tile shapes, tile coordinates and UpdateTileMappings calls.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Device.h"
#include "D3D12Sparse.h"
#include "D3D12Textures.h"
#include "Support/SparseKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;
using rojoRHI::test::SparseKit;

namespace {
    /// Heap bytes of one tile.
    constexpr uint64_t kTile = 65536;

    /// The warnings and errors `countWarning` has seen.
    std::atomic<uint64_t> g_warnings = 0;

    //==================================================================================================================
    /// Counts `severity` when it is a warning or an error, the debug layer's among them, and writes it to stderr, since the
    /// router aborts right after an error and the count would never be read. It may run on any thread.
    void countWarning(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity != MessageSeverity::Info)
        {
            ++g_warnings;
            std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
            std::fflush(stderr);
        }
    }

    /// Counts the library's warnings and errors into `g_warnings`, reset first, while it lives, and removes the callback when it
    /// goes, however the case ends.
    class WarningCount
    {
      public:
        WarningCount();
        ~WarningCount();

        //==============================================================================================================
        WarningCount(const WarningCount&) = delete;

        //==============================================================================================================
        WarningCount& operator=(const WarningCount&) = delete;
    };

    //==================================================================================================================
    WarningCount::WarningCount()
    {
        g_warnings = 0;
        setMessageCallback(countWarning, nullptr);
    }

    //==================================================================================================================
    WarningCount::~WarningCount()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    /// Returns the native resource of `texture`, a live texture of `device`.
    ID3D12Resource* nativeOf(Device device, Texture texture)
    {
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native resource of `buffer`, a live buffer of `device`.
    ID3D12Resource* nativeOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    /// What `GetResourceTiling` reports of a reserved resource.
    struct Tiling
    {
        UINT tiles = 0;                    ///< Tiles of the whole resource.
        D3D12_PACKED_MIP_INFO packed = {}; ///< The packed mips.
        D3D12_TILE_SHAPE shape = {};       ///< The shape of a tile of the standard mips.
    };

    //==================================================================================================================
    /// Returns what `GetResourceTiling` reports of `resource` on `device`.
    Tiling tilingOf(Device device, ID3D12Resource* resource)
    {
        Tiling tiling;
        DeviceImpl::of(device).device()->GetResourceTiling(resource, &tiling.tiles, &tiling.packed, &tiling.shape, nullptr, 0, nullptr);
        return tiling;
    }

    //==================================================================================================================
    /// Returns a record of a 2D array texture of 512 square texels, 10 mips and 2 layers, measured as 128 by 128 tiles with
    /// a tail from mip 3 of `tailTiles` tiles per layer.
    core::TextureRecord arrayRecord(uint32_t tailTiles)
    {
        core::TextureRecord record;
        record.type = TextureType::Tex2DArray;
        record.format = Format::RGBA8Unorm;
        record.extent = { 512, 512, 1 };
        record.mips = 10;
        record.layers = 2;
        record.usage = TextureUsage::Sampled | TextureUsage::Sparse;
        record.tiles = TileShape{ .width = 128, .height = 128, .depth = 1, .bytes = 65536, .mipTailFirst = 3, .mipTailBytes = tailTiles * kTile };
        return record;
    }

    //==================================================================================================================
    /// Checks that `region` starts at tile (`x`, `y`, `z`) of `subresource` and, from heap tile `heapStart`, maps a box of
    /// `width` by `height` by `depth` tiles.
    void checkBox(const d3d12::TileRegion& region, UINT subresource, std::array<UINT, 3> origin, std::array<UINT, 3> size, UINT heapStart)
    {
        CHECK(region.start.Subresource == subresource);
        CHECK(region.start.X == origin[0]);
        CHECK(region.start.Y == origin[1]);
        CHECK(region.start.Z == origin[2]);
        CHECK(region.size.UseBox == TRUE);
        CHECK(region.size.Width == size[0]);
        CHECK(UINT{ region.size.Height } == size[1]);
        CHECK(UINT{ region.size.Depth } == size[2]);
        CHECK(region.size.NumTiles == size[0] * size[1] * size[2]);
        CHECK(region.heapStart == heapStart);
    }
} // namespace

//======================================================================================================================
TEST_CASE("tileCoordinates maps a region of two layers to one coordinate per layer", "[d3d12]")
{
    const core::TextureRecord record = arrayRecord(1);
    // Mip 1 is 256 square texels, 2 by 2 tiles; the region is its right column, 1 by 2 tiles, over both layers, from heap
    // tile 5.
    const TileMapping mapping{ .texture = Texture{ 1 },
                               .region = { .mip = 1, .layer = 0, .layers = 2, .offset = { 128, 0, 0 }, .extent = { 128, 256, 1 } },
                               .heap = Heap{ 1 },
                               .heapOffset = 5 * kTile };
    const std::vector<d3d12::TileRegion> regions = d3d12::tileCoordinates(record, mapping);
    REQUIRE(regions.size() == 2);
    // Subresource mip + layer * mips; layer 1's range follows layer 0's two tiles.
    checkBox(regions[0], 1, { 1, 0, 0 }, { 1, 2, 1 }, 5);
    checkBox(regions[1], 11, { 1, 0, 0 }, { 1, 2, 1 }, 7);

    TileMapping removal = mapping;
    removal.heap = {};
    removal.heapOffset = 0;
    removal.unmap = true;
    const std::vector<d3d12::TileRegion> unmapped = d3d12::tileCoordinates(record, removal);
    REQUIRE(unmapped.size() == 2);
    checkBox(unmapped[0], 1, { 1, 0, 0 }, { 1, 2, 1 }, 0);
    checkBox(unmapped[1], 11, { 1, 0, 0 }, { 1, 2, 1 }, 0);
}

//======================================================================================================================
TEST_CASE("tileCoordinates maps the mip tail of layer 1 alone at its first packed mip", "[d3d12]")
{
    const core::TextureRecord record = arrayRecord(2);
    const TileMapping mapping{
        .texture = Texture{ 1 }, .region = { .mip = 3, .layer = 1, .layers = 1 }, .mipTail = true, .heap = Heap{ 1 }, .heapOffset = 9 * kTile
    };
    const std::vector<d3d12::TileRegion> regions = d3d12::tileCoordinates(record, mapping);
    REQUIRE(regions.size() == 1);
    // The first packed mip of layer 1 is subresource 3 + 1 * 10; the tail's tiles run along x from 0, not as a box.
    CHECK(regions[0].start.Subresource == 13u);
    CHECK(regions[0].start.X == 0u);
    CHECK(regions[0].start.Y == 0u);
    CHECK(regions[0].start.Z == 0u);
    CHECK(regions[0].size.UseBox == FALSE);
    CHECK(regions[0].size.NumTiles == 2u);
    CHECK(regions[0].heapStart == 9u);
}

//======================================================================================================================
TEST_CASE("tileCoordinates maps a buffer range from its first tile", "[d3d12]")
{
    // Two whole tiles and the 100 bytes of a last partial one, from tile 3 of the buffer onto heap tile 4.
    const TileMapping mapping{ .buffer = Buffer{ 1 }, .bufferOffset = 3 * kTile, .bufferSize = 2 * kTile + 100, .heap = Heap{ 1 }, .heapOffset = 4 * kTile };
    const d3d12::TileRegion region = d3d12::tileCoordinates(mapping);
    CHECK(region.start.Subresource == 0u);
    CHECK(region.start.X == 3u);
    CHECK(region.start.Y == 0u);
    CHECK(region.start.Z == 0u);
    CHECK(region.size.UseBox == FALSE);
    CHECK(region.size.NumTiles == 3u);
    CHECK(region.heapStart == 4u);
}

//======================================================================================================================
TEST_CASE("placementSparse follows the tiled-resources tier", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    REQUIRE(SUCCEEDED(impl.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, static_cast<UINT>(sizeof(options)))));
    UNSCOPED_INFO("TiledResourcesTier " << static_cast<int>(options.TiledResourcesTier));
    CHECK(impl.features.tiledResourcesTier == static_cast<uint32_t>(options.TiledResourcesTier));
    CHECK(created->capabilities().placementSparse == (options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a sparse texture's tile shape matches GetResourceTiling and the standard extent", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    REQUIRE(created->capabilities().placementSparse);
    uint32_t reported = 0;
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const auto format = static_cast<Format>(value);
        CAPTURE(value);
        if (!created->formatCapabilities(format).sparse)
        {
            continue;
        }
        ++reported;
        // A full mip chain, so the small mips pack into a tail.
        Result<Texture> texture = created->createTexture(
            TextureDesc{ .format = format, .extent = { 512, 512, 1 }, .mips = 10, .usage = TextureUsage::Sparse, .label = "test.sparse.format" }
        );
        INFO("createTexture: " << (texture ? std::string("ok") : texture.error().message));
        REQUIRE(texture.has_value());
        const TileShape shape = created->tileShape(*texture);
        const Tiling tiling = tilingOf(*created, nativeOf(*created, *texture));
        CHECK(shape.width == tiling.shape.WidthInTexels);
        CHECK(shape.height == tiling.shape.HeightInTexels);
        CHECK(shape.depth == tiling.shape.DepthInTexels);
        CHECK(shape.bytes == kTile);
        const bool tail = tiling.packed.NumPackedMips != 0;
        CHECK(shape.mipTailFirst == (tail ? uint32_t{ tiling.packed.NumStandardMips } : 10u));
        CHECK(shape.mipTailBytes == (tail ? uint64_t{ tiling.packed.NumTilesForPackedMips } * kTile : 0u));
        const core::TileExtent standard = core::standardTileExtent(format, TextureType::Tex2D);
        CHECK(shape.width == standard.width);
        CHECK(shape.height == standard.height);
        CHECK(shape.depth == standard.depth);
        created->destroy(*texture);
    }
    CHECK(reported > 0);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a one-dimensional sparse texture fails creation with Unsupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    REQUIRE(created->capabilities().placementSparse);
    const Result<Texture> texture = created->createTexture(
        TextureDesc{ .type = TextureType::Tex1D, .format = Format::RGBA8Unorm, .extent = { 1024, 1, 1 }, .usage = TextureUsage::Sparse }
    );
    REQUIRE_FALSE(texture.has_value());
    CHECK(texture.error().category == ErrorCategory::Unsupported);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a sparse buffer is a reserved resource rounded up to whole tiles", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    REQUIRE(created->capabilities().placementSparse);
    Result<Buffer> buffer = created->createBuffer(BufferDesc{ .size = 4 * kTile + 100, .usage = BufferUsage::Sparse, .label = "test.sparse.odd" });
    REQUIRE(buffer.has_value());
    ID3D12Resource* resource = nativeOf(*created, *buffer);
    CHECK(resource->GetDesc().Width == 5 * kTile);
    // Only a reserved resource has tiles of its own.
    CHECK(tilingOf(*created, resource).tiles == 5u);
    CHECK(created->tileShape(*buffer).bytes == kTile);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("mapTiles counts one submit call per resource and heap group", "[d3d12]")
{
    SparseKit kit;
    // The kit texture's tail is GetResourceTiling's packed mips, or none when the device packs no mip (WARP packs none).
    const Tiling tiling = tilingOf(kit.device, nativeOf(kit.device, kit.texture));
    const bool packed = tiling.packed.NumPackedMips != 0;
    CHECK(kit.shape.mipTailFirst == (packed ? uint32_t{ tiling.packed.NumStandardMips } : SparseKit::kMips));
    CHECK(kit.shape.mipTailBytes == (packed ? uint64_t{ tiling.packed.NumTilesForPackedMips } * kTile : 0u));
    // Region tiles of layer 0 and the tail of layer 1 alone, or without a tail the first tile of layer 1, share a resource
    // and a heap: one call; the buffer range is another.
    TileMapping second = kit.firstTile();
    if (packed)
    {
        second = kit.tail();
    }
    second.region.layer = 1;
    second.heapOffset = 16 * kTile;
    const std::array mappings = { kit.firstTile(), second, kit.bufferTiles(2, 20 * kTile) };
    const uint64_t before = kit.device.counters().nativeSubmitCalls;
    kit.queue.mapTiles(mappings);
    CHECK(kit.device.counters().nativeSubmitCalls == before + 2);
    // Unmapping all three makes one null-range call per resource.
    const std::array removals = { SparseKit::removal(mappings[0]), SparseKit::removal(mappings[1]), SparseKit::removal(mappings[2]) };
    kit.queue.mapTiles(removals);
    CHECK(kit.device.counters().nativeSubmitCalls == before + 4);
    // An empty call issues nothing.
    kit.queue.mapTiles({});
    CHECK(kit.device.counters().nativeSubmitCalls == before + 4);
    destroyDevice(kit.device);
}

//======================================================================================================================
TEST_CASE("every queue type maps and unmaps a tile", "[d3d12]")
{
    const WarningCount warnings;
    SparseKit kit;
    // The debug layer counts a mapping as a write of the resource, so each queue's mappings are ordered after the previous
    // queue's by a fence, as the application orders other work on a resource across queues.
    Result<Fence> order = kit.device.createFence(0, "test.sparse.order");
    REQUIRE(order.has_value());
    uint64_t value = 0;
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        CAPTURE(static_cast<int>(type));
        Queue queue = kit.device.queue(type);
        if (value > 0)
        {
            queue.submit({}, std::array{ FenceValue{ *order, value } }, {});
        }
        queue.mapTiles(std::array{ kit.firstTile(), kit.bufferTiles(1, kTile) });
        queue.mapTiles(std::array{ SparseKit::removal(kit.firstTile()), SparseKit::removal(kit.bufferTiles(1, kTile)) });
        ++value;
        queue.submit({}, {}, std::array{ FenceValue{ *order, value } });
    }
    // Device destruction idles every queue, so a message the mappings raised on the GPU timeline is counted too.
    destroyDevice(kit.device);
    CHECK(g_warnings.load() == 0);
}
