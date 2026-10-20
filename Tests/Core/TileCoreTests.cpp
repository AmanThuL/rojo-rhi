//----------------------------------------------------------------------------------------------------------------------
/// @file TileCoreTests.cpp
/// @brief Tests the tile and byte events and the first-use marks the validation tracker keeps for sparse resources, and its frame join query.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Tracker/FirstUse.h"
#include "Core/Tracker/Tiles.h"
#include "Core/Tracker/Tracker.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

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
TEST_CASE("tile marks reach an event of a newly mapped box, and an event of the whole subresource", "[core]")
{
    core::TileMarks marks;
    CHECK_FALSE(marks.reaches(std::nullopt));
    marks.mark(box(128, 0, 128, 128));
    CHECK(marks.reaches(box(130, 10, 4, 4)));
    CHECK(marks.reaches(std::nullopt));
    // A box that misses every mark is not reached.
    CHECK_FALSE(marks.reaches(box(0, 0, 128, 128)));
    CHECK_FALSE(marks.reaches(box(256, 0, 128, 128)));
    // A remap of a box the mark contains keeps one mark.
    marks.mark(box(128, 0, 128, 128));
    CHECK(marks.count() == 1);
    marks.mark(box(0, 0, 256, 128));
    CHECK(marks.count() == 1);
    marks.clear(std::nullopt);
    CHECK_FALSE(marks.reaches(std::nullopt));
    CHECK(marks.count() == 0);
}

//======================================================================================================================
TEST_CASE("an unmap clears exactly the texels of the tile marks it names", "[core]")
{
    core::TileMarks marks;
    marks.mark(box(0, 0, 256, 256));
    marks.clear(box(128, 128, 128, 128));
    CHECK(marks.reaches(box(0, 0, 128, 128)));
    CHECK(marks.reaches(box(128, 0, 128, 128)));
    CHECK(marks.reaches(box(0, 128, 128, 128)));
    CHECK_FALSE(marks.reaches(box(128, 128, 128, 128)));
    marks.clear(box(0, 0, 256, 128));
    marks.clear(box(0, 128, 128, 128));
    CHECK_FALSE(marks.reaches(std::nullopt));
    CHECK(marks.count() == 0);
}

//======================================================================================================================
TEST_CASE("a whole-subresource tile mark reaches every event until a whole clear", "[core]")
{
    core::TileMarks marks;
    marks.mark(std::nullopt);
    CHECK(marks.reaches(box(0, 0, 1, 1)));
    CHECK(marks.reaches(std::nullopt));
    marks.clear(std::nullopt);
    CHECK_FALSE(marks.reaches(box(0, 0, 1, 1)));
}

//======================================================================================================================
TEST_CASE("subtracting a box leaves the texels outside it as disjoint boxes", "[core]")
{
    const core::TexelBox from{ .offset = { 0, 0, 0 }, .extent = { 4, 4, 4 } };
    const core::TexelBox cut{ .offset = { 1, 1, 1 }, .extent = { 2, 2, 2 } };
    const std::vector<core::TexelBox> rest = core::subtract(from, cut);
    CHECK(rest.size() == 6);
    uint64_t texels = 0;
    for (size_t index = 0; index < rest.size(); ++index)
    {
        texels += uint64_t{ rest[index].extent.width } * rest[index].extent.height * rest[index].extent.depth;
        CHECK_FALSE(core::overlaps(rest[index], cut));
        for (size_t other = index + 1; other < rest.size(); ++other)
        {
            CHECK_FALSE(core::overlaps(rest[index], rest[other]));
        }
    }
    CHECK(texels == 64 - 8);
    // A cut that misses leaves the box; one that contains it leaves nothing.
    CHECK(core::subtract(from, core::TexelBox{ .offset = { 4, 0, 0 }, .extent = { 4, 4, 4 } }).size() == 1);
    CHECK(core::subtract(cut, from).empty());
}

//======================================================================================================================
TEST_CASE("byte marks clear at a use that a global barrier after the mapping orders", "[core]")
{
    constexpr core::Scope kCopyWrite{ .stage = Stage::Copy, .access = Access::CopyWrite };
    core::ByteMarks marks;
    marks.mark(0, 65536, QueueType::Graphics, 4);
    CHECK(marks.count() == 1);
    // No barrier since the mapping: the use is reported.
    const std::optional<core::ByteMark> missing = marks.firstUse(0, 256, QueueType::Graphics, kCopyWrite);
    REQUIRE(missing.has_value());
    CHECK(missing->queue == QueueType::Graphics);
    CHECK(missing->position == 4);
    // A barrier whose destination misses the use's access orders nothing for it.
    marks.reach(QueueType::Graphics, core::Clock{}, core::Scope{ .stage = Stage::Copy, .access = Access::CopyRead });
    CHECK(marks.firstUse(0, 256, QueueType::Graphics, kCopyWrite).has_value());
    // Another queue's barrier, which no clock orders after the mapping, orders nothing either.
    marks.reach(QueueType::Compute, core::Clock{}, kCopyWrite);
    CHECK(marks.firstUse(0, 256, QueueType::Compute, kCopyWrite).has_value());
    marks.reach(QueueType::Graphics, core::Clock{}, kCopyWrite);
    CHECK_FALSE(marks.firstUse(0, 256, QueueType::Graphics, kCopyWrite).has_value());
    // The use cleared the marks of its bytes alone; the rest of the tile keeps the barriers it gained.
    CHECK(marks.count() == 1);
    CHECK_FALSE(marks.firstUse(256, 65536, QueueType::Graphics, kCopyWrite).has_value());
    CHECK(marks.count() == 0);
}

//======================================================================================================================
TEST_CASE("byte marks gain another queue's barrier only when its clock orders the mapping", "[core]")
{
    constexpr core::Scope kCopyWrite{ .stage = Stage::Copy, .access = Access::CopyWrite };
    core::ByteMarks marks;
    marks.mark(0, 65536, QueueType::Compute, 7);
    core::Clock ordered{};
    ordered[static_cast<size_t>(QueueType::Compute)] = 6;
    marks.reach(QueueType::Graphics, ordered, kCopyWrite);
    CHECK(marks.firstUse(0, 65536, QueueType::Graphics, kCopyWrite).has_value());
    ordered[static_cast<size_t>(QueueType::Compute)] = 7;
    marks.reach(QueueType::Graphics, ordered, kCopyWrite);
    CHECK_FALSE(marks.firstUse(0, 65536, QueueType::Graphics, kCopyWrite).has_value());
}

//======================================================================================================================
TEST_CASE("a remap of buffer bytes needs a new barrier, and an unmap clears their marks", "[core]")
{
    constexpr core::Scope kCopyWrite{ .stage = Stage::Copy, .access = Access::CopyWrite };
    core::ByteMarks marks;
    marks.mark(0, 131072, QueueType::Graphics, 2);
    marks.reach(QueueType::Graphics, core::Clock{}, kCopyWrite);
    marks.mark(65536, 131072, QueueType::Graphics, 5);
    CHECK(marks.count() == 2);
    CHECK_FALSE(marks.firstUse(0, 65536, QueueType::Graphics, kCopyWrite).has_value());
    CHECK(marks.firstUse(65536, 131072, QueueType::Graphics, kCopyWrite).has_value());
    marks.clear(65536, 131072);
    CHECK(marks.count() == 0);
    CHECK_FALSE(marks.firstUse(0, 131072, QueueType::Graphics, kCopyWrite).has_value());
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
