//----------------------------------------------------------------------------------------------------------------------
/// @file TileCoreTests.cpp
/// @brief Tests the tile and byte events the validation tracker keeps for sparse resources, and its frame join query.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Tracker/Tiles.h"
#include "Core/Tracker/Tracker.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using namespace rojoRHI;
using test::PassKit;

namespace {
    //==================================================================================================================
    /// Returns the box of `width` by `height` texels from (`x`, `y`) in one slice.
    core::TexelBox box(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        return core::TexelBox{ .offset = { x, y, 0 }, .extent = { width, height, 1 } };
    }
} // namespace

//======================================================================================================================
TEST_CASE("texel boxes overlap only when they share a texel on every axis", "[core]")
{
    CHECK(core::overlaps(box(0, 0, 128, 128), box(64, 64, 128, 128)));
    CHECK_FALSE(core::overlaps(box(0, 0, 128, 128), box(128, 0, 128, 128)));
    CHECK_FALSE(core::overlaps(box(0, 0, 128, 128), box(0, 128, 128, 128)));
    CHECK_FALSE(core::overlaps(core::TexelBox{ .offset = { 0, 0, 1 }, .extent = { 4, 4, 1 } }, box(0, 0, 4, 4)));
}

//======================================================================================================================
TEST_CASE("tile events report the latest event of the tiles a query names", "[core]")
{
    core::TileEvents events;
    CHECK(events.latest(std::nullopt) == 0);
    events.note(box(0, 0, 128, 128), 3, 0);
    events.note(box(128, 0, 128, 128), 5, 0);
    CHECK(events.latest(box(0, 0, 16, 16)) == 3);
    CHECK(events.latest(box(128, 64, 16, 16)) == 5);
    CHECK(events.latest(box(256, 0, 128, 128)) == 0);
    // A query of the whole subresource sees every part.
    CHECK(events.latest(std::nullopt) == 5);
    // An event of the whole subresource is the latest of every tile and replaces the parts before it.
    events.note(std::nullopt, 7, 0);
    CHECK(events.latest(box(256, 0, 128, 128)) == 7);
    CHECK(events.parts() == 0);
}

//======================================================================================================================
TEST_CASE("tile events forget parts a later part contains and events at or below the floor", "[core]")
{
    core::TileEvents events;
    events.note(box(0, 0, 128, 128), 2, 0);
    events.note(box(0, 0, 256, 256), 4, 0);
    CHECK(events.parts() == 1);
    events.note(box(256, 0, 128, 128), 6, 0);
    CHECK(events.parts() == 2);
    // The CPU observed everything through position 4 complete: those events order nothing more.
    events.note(box(512, 0, 128, 128), 8, 4);
    CHECK(events.parts() == 2);
    CHECK(events.latest(box(0, 0, 16, 16)) == 0);
    CHECK(events.latest(std::nullopt) == 8);
}

//======================================================================================================================
TEST_CASE("byte events report the latest event of a range and merge what the floor forgets", "[core]")
{
    core::ByteEvents events;
    CHECK(events.latest(0, 65536) == 0);
    events.note(0, 65536, 3, 0);
    events.note(65536, 131072, 5, 0);
    CHECK(events.latest(0, 256) == 3);
    CHECK(events.latest(65000, 66000) == 5);
    CHECK(events.latest(131072, 196608) == 0);
    events.note(196608, 262144, 9, 5);
    CHECK(events.latest(0, 196608) == 0);
    CHECK(events.latest(0, 262144) == 9);
    // Everything from zero to the last event is one range of no event, then the event, then the rest.
    CHECK(events.ranges() == 3);
}

//======================================================================================================================
TEST_CASE("a mip-tail mapping changes every tail mip of its layers and nothing else", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 64 * 65536, .memory = Memory::Gpu, .label = "test.heap" });
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex2DArray,
                     .format = Format::RGBA8Unorm,
                     .extent = { 512, 512, 1 },
                     .mips = 10,
                     .layers = 2,
                     .usage = TextureUsage::Sampled | TextureUsage::Sparse,
                     .label = "test.sparse" }
    );
    const TileShape shape = kit.device.tileShape(texture);
    REQUIRE(shape.mipTailFirst > 0);
    REQUIRE(shape.mipTailFirst < 10);
    const TileMapping tail{ .texture = texture, .region = { .mip = shape.mipTailFirst, .layer = 1 }, .mipTail = true, .heap = heap };
    kit.device.queue(QueueType::Graphics).mapTiles(std::array{ tail });
    for (uint32_t mip = 0; mip < 10; ++mip)
    {
        const bool inTail = mip >= shape.mipTailFirst;
        CHECK((tracker.lastMapping(texture, mip, 1, QueueType::Graphics).value_or(0) != 0) == inTail);
        CHECK(tracker.lastMapping(texture, mip, 0, QueueType::Graphics).value_or(0) == 0);
        CHECK(tracker.lastMapping(texture, mip, 1, QueueType::Compute).value_or(0) == 0);
    }
    kit.list.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ kit.list });
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a submission with no lists to the compute queue stays unjoined until a graphics submission waits on it", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    CHECK(tracker.unjoined() == std::nullopt);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> done{ FenceValue{ fence, 1 } };
    kit.device.queue(QueueType::Compute).submit({}, {}, done);
    CHECK(tracker.unjoined() == QueueType::Compute);
    kit.device.queue(QueueType::Graphics).submit({}, done);
    CHECK(tracker.unjoined() == std::nullopt);
    kit.list.end();
    kit.device.queue(QueueType::Graphics).submit(std::array{ kit.list });
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}
