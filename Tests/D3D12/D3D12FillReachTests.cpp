//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12FillReachTests.cpp
/// @brief Tests through the public entries the words D3D12 fills write on both sides of the raw view's reach and fills in a row.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The bytes a buffer's own raw view reaches, 2^29: a fill writes the words past it through a view of their own.
    constexpr uint64_t kReach = uint64_t{ 1 } << 29;

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Records on `list` a global barrier from the copy writes of fills to the copy-stage `access`.
    void afterFills(CommandList list, Access access)
    {
        const std::array<GlobalBarrier, 1> barriers{ GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = access } };
        list.barrier(barriers);
    }

    //==================================================================================================================
    /// Records on `list`, a Graphics list of `device`'s open frame, a barrier after its fills and a copy of `size` bytes of
    /// `source` from `offset` into a new `Readback` buffer, submits it, ends the frame, waits for the list and returns the
    /// bytes the copy wrote.
    std::vector<std::byte> readBack(Device device, CommandList list, Buffer source, uint64_t offset, uint64_t size)
    {
        Result<Buffer> created = device.createBuffer({ .size = size, .memory = Memory::Readback, .label = "test.readback" });
        REQUIRE(created.has_value());
        const Buffer readback = *created;
        Result<Fence> fence = device.createFence(0, "test.done");
        REQUIRE(fence.has_value());
        const Fence done = *fence;
        afterFills(list, Access::CopyRead);
        list.copyBuffer(source, offset, readback, 0, size);
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        device.endFrame();
        REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
        const std::byte* mapped = device.cpuPointer(readback);
        REQUIRE(mapped != nullptr);
        std::vector<std::byte> bytes(mapped, mapped + size);
        device.destroy(done);
        device.destroy(readback);
        return bytes;
    }

    //==================================================================================================================
    /// Writes `word` in little-endian byte order over the bytes of `bytes` from `offset`, `size` bytes, both multiples of 4.
    void writeWords(std::vector<std::byte>& bytes, uint64_t offset, uint64_t size, uint32_t word)
    {
        for (uint64_t at = offset; at < offset + size; ++at)
        {
            bytes[at] = static_cast<std::byte>((word >> ((at % 4) * 8)) & 0xFFu);
        }
    }

    //==================================================================================================================
    /// Returns the index of the first byte where `read` and `expected` differ, or their size when they are equal.
    size_t firstDifference(const std::vector<std::byte>& read, const std::vector<std::byte>& expected)
    {
        REQUIRE(read.size() == expected.size());
        for (size_t i = 0; i < read.size(); ++i)
        {
            if (read[i] != expected[i])
            {
                return i;
            }
        }
        return read.size();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a fill across the raw view's 512 MiB reach writes every word on both sides", "[d3d12]")
{
    constexpr uint64_t kSize = kReach + (uint64_t{ 1 } << 16);
    constexpr uint64_t kFillOffset = kReach - (uint64_t{ 1 } << 15);
    constexpr uint64_t kFillSize = uint64_t{ 1 } << 16;
    constexpr uint64_t kReadOffset = kReach - (uint64_t{ 1 } << 16);
    constexpr uint64_t kReadSize = uint64_t{ 1 } << 17;
    constexpr uint32_t kPattern = 0xA1B2C3D4u;
    Result<Device> device = createDevice({});
    REQUIRE(device.has_value());
    Result<Buffer> created = device->createBuffer({ .size = kSize, .usage = BufferUsage::ShaderWrite, .label = "test.large" });
    if (!created && created.error().category == ErrorCategory::OutOfMemory)
    {
        destroyDevice(*device);
        SKIP("the adapter cannot hold a buffer of 512 MiB and 64 KiB: " << created.error().message);
    }
    REQUIRE(created.has_value());
    const Buffer buffer = *created;
    device->beginFrame();
    CommandList list = device->acquireCommandList(QueueType::Graphics, "test.reach");
    list.fillBuffer(buffer, 0, kSize, 0);
    afterFills(list, Access::CopyWrite);
    const uint64_t calls = recordCalls(list);
    const uint64_t writes = device->counters().descriptorWrites;
    list.fillBuffer(buffer, kFillOffset, kFillSize, kPattern);
    // The kernel the first fill bound stays bound: each window sets its constants and dispatches, the upper one through a
    // view written for it.
    CHECK(recordCalls(list) - calls == 4u);
    CHECK(device->counters().descriptorWrites - writes == 1u);
    std::vector<std::byte> expected(kReadSize);
    writeWords(expected, kFillOffset - kReadOffset, kFillSize, kPattern);
    CHECK(firstDifference(readBack(*device, list, buffer, kReadOffset, kReadSize), expected) == kReadSize);
    device->destroy(buffer);
    destroyDevice(*device);
}

//======================================================================================================================
TEST_CASE("a fill after a fill reuses the bound kernel and both write their own ranges", "[d3d12]")
{
    constexpr uint64_t kSize = 1024;
    std::vector<std::byte> initial(kSize);
    for (uint64_t i = 0; i < kSize; ++i)
    {
        initial[i] = static_cast<std::byte>(i * 7 % 251 + 1);
    }
    Result<Device> device = createDevice({});
    REQUIRE(device.has_value());
    Result<Buffer> created = device->createBuffer({ .size = kSize, .usage = BufferUsage::ShaderWrite, .label = "test.fills" }, initial);
    REQUIRE(created.has_value());
    const Buffer buffer = *created;
    device->beginFrame();
    CommandList list = device->acquireCommandList(QueueType::Graphics, "test.fills");
    const uint64_t first = recordCalls(list);
    list.fillBuffer(buffer, 64, 256, 0x01020304u);
    CHECK(recordCalls(list) - first == 4u);
    // The two fills write disjoint ranges, so they need no barrier between them.
    const uint64_t second = recordCalls(list);
    list.fillBuffer(buffer, 512, 132, 0x0A0B0C0Du);
    CHECK(recordCalls(list) - second == 2u);
    std::vector<std::byte> expected = initial;
    writeWords(expected, 64, 256, 0x01020304u);
    writeWords(expected, 512, 132, 0x0A0B0C0Du);
    CHECK(firstDifference(readBack(*device, list, buffer, 0, kSize), expected) == kSize);
    device->destroy(buffer);
    destroyDevice(*device);
}
