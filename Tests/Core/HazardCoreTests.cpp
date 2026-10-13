//----------------------------------------------------------------------------------------------------------------------
/// @file HazardCoreTests.cpp
/// @brief Tests the byte segments the validation tracker keeps per buffer and which buffers it follows.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Tracker/ByteSegments.h"
#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/Tracker.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A copy's write.
    constexpr core::Scope kCopyWrite{ .stage = Stage::Copy, .access = Access::CopyWrite };
    /// A merge floor above every position the tests use.
    constexpr std::array<uint64_t, 3> kFloor{ 100, 100, 100 };

    //==================================================================================================================
    /// Returns the first and one-past-last byte of each segment of `segments`, in order.
    std::vector<std::pair<uint64_t, uint64_t>> bounds(const core::ByteSegments& segments)
    {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        for (const core::ByteSegment& segment : segments.segments())
        {
            out.emplace_back(segment.begin, segment.end);
        }
        return out;
    }

    //==================================================================================================================
    /// Notes a copy's write at `position` on the graphics queue in every segment of `segments`.
    void write(std::span<core::ByteSegment> segments, uint64_t position)
    {
        for (core::ByteSegment& segment : segments)
        {
            core::recordUse(segment.hazards.on(QueueType::Graphics), position, kCopyWrite);
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("a new buffer is one segment, and a range splits it where the range starts and ends", "[core]")
{
    core::ByteSegments segments(1024);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 1024 } });
    const std::span<core::ByteSegment> middle = segments.isolate(256, 512);
    REQUIRE(middle.size() == 1);
    CHECK(middle.front().begin == 256);
    CHECK(middle.front().end == 512);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 256 }, { 256, 512 }, { 512, 1024 } });
    // A range across existing boundaries splits only at its own ends and returns every segment inside it.
    CHECK(segments.isolate(128, 768).size() == 3);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 128 }, { 128, 256 }, { 256, 512 }, { 512, 768 }, { 768, 1024 } });
    // A range on existing boundaries splits nothing.
    CHECK(segments.isolate(0, 1024).size() == 5);
}

//======================================================================================================================
TEST_CASE("neighbouring segments merge only when their hazard states are alike and older than the current command", "[core]")
{
    core::ByteSegments segments(1024);
    segments.isolate(256, 512);
    segments.merge(256, 512, kFloor);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 1024 } });
    write(segments.isolate(256, 512), 1);
    segments.merge(256, 512, kFloor);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 256 }, { 256, 512 }, { 512, 1024 } });
    // A write alike to the middle one, by a later command, merges with it once both are older than the current command.
    write(segments.isolate(512, 768), 2);
    segments.merge(512, 768, std::array<uint64_t, 3>{ 2, 100, 100 });
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 256 }, { 256, 512 }, { 512, 768 }, { 768, 1024 } });
    segments.merge(512, 768, kFloor);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 256 }, { 256, 768 }, { 768, 1024 } });
    // The same write over the whole buffer leaves every byte in one state again.
    write(segments.isolate(0, 1024), 3);
    segments.merge(0, 1024, kFloor);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 1024 } });
}

//======================================================================================================================
TEST_CASE("reaches compare as sets when segments merge", "[core]")
{
    constexpr core::Scope kRead{ .stage = Stage::Copy, .access = Access::CopyRead };
    constexpr core::Scope kShader{ .stage = Stage::ComputeShader, .access = Access::ShaderRead };
    core::ByteSegments segments(512);
    write(segments.isolate(0, 512), 1);
    // The two halves see the same two barriers in opposite orders.
    for (core::ByteSegment& segment : segments.isolate(0, 256))
    {
        core::recordBarrier(segment.hazards.on(QueueType::Graphics), 2, kCopyWrite, kRead);
        core::recordBarrier(segment.hazards.on(QueueType::Graphics), 3, kCopyWrite, kShader);
    }
    for (core::ByteSegment& segment : segments.isolate(256, 512))
    {
        core::recordBarrier(segment.hazards.on(QueueType::Graphics), 2, kCopyWrite, kShader);
        core::recordBarrier(segment.hazards.on(QueueType::Graphics), 3, kCopyWrite, kRead);
    }
    segments.merge(0, 512, kFloor);
    CHECK(bounds(segments) == std::vector<std::pair<uint64_t, uint64_t>>{ { 0, 512 } });
}

//======================================================================================================================
TEST_CASE("a ring buffer written at moving offsets over many commands keeps a bounded number of segments", "[core]")
{
    constexpr uint64_t kSlot = 256;
    constexpr uint64_t kSlots = 16;
    constexpr core::Scope kRead{ .stage = Stage::Copy, .access = Access::CopyRead };
    core::ByteSegments segments(kSlot * kSlots);
    size_t most = 0;
    uint64_t position = 0;
    for (uint64_t round = 0; round < 10 * kSlots; ++round)
    {
        const uint64_t begin = round % kSlots * kSlot;
        // A write, a barrier over the whole ring, and a read of the slot written, each its own command.
        ++position;
        write(segments.isolate(begin, begin + kSlot), position);
        segments.merge(begin, begin + kSlot, std::array<uint64_t, 3>{ position, position, position });
        ++position;
        for (core::ByteSegment& segment : segments.isolate(0, kSlot * kSlots))
        {
            core::recordBarrier(segment.hazards.on(QueueType::Graphics), position, kCopyWrite, kRead);
        }
        segments.merge(0, kSlot * kSlots, std::array<uint64_t, 3>{ position, position, position });
        ++position;
        for (core::ByteSegment& segment : segments.isolate(begin, begin + kSlot))
        {
            REQUIRE(core::checkUse(segment.hazards.on(QueueType::Graphics), position, kRead).kind == core::HazardKind::None);
            core::recordUse(segment.hazards.on(QueueType::Graphics), position, kRead);
        }
        segments.merge(begin, begin + kSlot, std::array<uint64_t, 3>{ position, position, position });
        most = std::max(most, segments.segments().size());
    }
    CHECK(most <= 4);
}

//======================================================================================================================
TEST_CASE("a write after a write chains through a read between two barriers, never through two records of one call", "[core]")
{
    constexpr core::Scope kCopyRead{ .stage = Stage::Copy, .access = Access::CopyRead };
    core::QueueHazards chained;
    core::recordUse(chained, 1, kCopyWrite);
    core::recordBarrier(chained, 2, kCopyWrite, kCopyRead);
    CHECK(core::checkUse(chained, 3, kCopyRead).kind == core::HazardKind::None);
    core::recordUse(chained, 3, kCopyRead);
    core::recordBarrier(chained, 4, kCopyRead, kCopyWrite);
    CHECK(core::checkUse(chained, 5, kCopyWrite).kind == core::HazardKind::None);
    // The same two records in one call, with no read between them, order nothing.
    core::QueueHazards batched;
    core::recordUse(batched, 1, kCopyWrite);
    core::recordBarrier(batched, 2, kCopyWrite, kCopyRead);
    core::recordBarrier(batched, 2, kCopyRead, kCopyWrite);
    CHECK(core::checkUse(batched, 3, kCopyWrite).kind == core::HazardKind::Unordered);
}

//======================================================================================================================
TEST_CASE("reads alike to one the state remembers are kept once", "[core]")
{
    constexpr core::Scope kShaderRead{ .stage = Stage::FragmentShader, .access = Access::ShaderRead };
    core::QueueHazards state;
    size_t most = 0;
    for (uint64_t position = 1; position <= 64; position += 2)
    {
        core::recordUse(state, position, kShaderRead);
        core::recordBarrier(state, position + 1, kShaderRead, kCopyWrite);
        most = std::max(most, state.reads.size());
    }
    // A read whose reach is the barrier still being replayed stays apart until a later barrier, so at most two remain.
    CHECK(most <= 2);
    CHECK(core::checkUse(state, 100, kCopyWrite).kind == core::HazardKind::None);
}

//======================================================================================================================
TEST_CASE("the tracker follows a buffer from creation to destruction, and never a frame-arena page", "[core]")
{
#ifndef NDEBUG
    PassKit kit(QueueType::Graphics, true);
    core::Tracker& tracker = core::coreOf(kit.device).tracker();
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    CHECK(tracker.segmentCount(buffer) == std::optional<size_t>(1));
    kit.list.fillBuffer(buffer, 256, 256, 0);
    const std::array<std::byte, 16> bytes{};
    kit.list.uploadFrameData(bytes);
    const Buffer page = core::coreOf(kit.list).page.buffer;
    CHECK(tracker.segmentCount(page) == std::nullopt);
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    // The write splits the buffer into the bytes before, inside and after it.
    CHECK(tracker.segmentCount(buffer) == std::optional<size_t>(3));
    kit.device.destroy(buffer);
    CHECK(tracker.segmentCount(buffer) == std::nullopt);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a placed buffer records its heap range for the tracker", "[core]")
{
    PassKit kit;
    const BufferDesc desc{ .size = 1024, .label = "test.placed" };
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    const uint64_t offset = 2 * kit.device.placementRequirements(desc).alignment;
    const Buffer buffer = *kit.device.createBuffer(desc, {}, Placement{ .heap = heap, .offset = offset });
    const core::BufferRecord* record = core::coreOf(kit.device).pool<Buffer>().checked(buffer, false);
    REQUIRE(record != nullptr);
    CHECK(record->heap == heap);
    CHECK(record->heapOffset == offset);
    kit.device.destroy(buffer);
    kit.device.destroy(heap);
    kit.finish();
}
