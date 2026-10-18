//----------------------------------------------------------------------------------------------------------------------
/// @file FillConformanceTests.cpp
/// @brief Observes `fillBuffer` on a GPU: the pattern it writes, in little-endian order, and the bytes outside its range.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The size of a slow write: long enough that a later write left unordered would finish first.
    constexpr uint64_t kBig = uint64_t{ 32 } << 20;

    /// The bytes each ordering case writes second and reads back, at the end of the slow write's range.
    constexpr uint64_t kTail = 256;

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes; fills are copy writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }

    //==================================================================================================================
    /// Returns the last `kTail` bytes of the `kBig`-byte `source`, copied into a new Readback buffer by a list of its own.
    std::vector<std::byte> readTail(test::GpuKit& kit, Buffer source)
    {
        const Buffer readback = kit.buffer(kTail, Memory::Readback);
        CommandList list = kit.list(QueueType::Graphics, "test.readTail");
        list.copyBuffer(source, kBig - kTail, readback, 0, kTail);
        kit.run(std::array{ list });
        return kit.read(readback);
    }

    //==================================================================================================================
    /// Returns `bytes` with `pattern` written in little-endian byte order over `size` bytes from `offset`, as `fillBuffer`
    /// writes it.
    std::vector<std::byte> filled(std::vector<std::byte> bytes, uint64_t offset, uint64_t size, uint32_t pattern)
    {
        for (uint64_t i = 0; i < size; ++i)
        {
            bytes[offset + i] = static_cast<std::byte>(pattern >> (8 * (i % 4)));
        }
        return bytes;
    }
} // namespace

//======================================================================================================================
TEST_CASE("fillBuffer with four equal bytes writes them over exactly the range", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> background = test::pattern(512, 1);
    const Buffer buffer = kit.buffer(512, Memory::Gpu, BufferUsage::ShaderWrite, background);
    CommandList list = kit.list(QueueType::Graphics, "test.fill");
    list.fillBuffer(buffer, 64, 256, 0x7E7E7E7Eu);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(buffer) == filled(background, 64, 256, 0x7E7E7E7Eu));
}

//======================================================================================================================
TEST_CASE("fillBuffer with an unequal pattern writes it in little-endian order over exactly the range", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> background = test::pattern(1024, 2);
    const Buffer dedicated = kit.buffer(1024, Memory::Gpu, BufferUsage::ShaderWrite, background);
    // A placed buffer one alignment into its heap, so its addresses do not start at the heap's start.
    const PlacementRequirements requirements = kit.device.placementRequirements(BufferDesc{ .size = 1024, .usage = BufferUsage::ShaderWrite });
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = requirements.alignment + requirements.size, .label = "test.heap" });
    const Buffer placed = kit.buffer(1024, Memory::Gpu, BufferUsage::ShaderWrite, background, Placement{ heap, requirements.alignment });
    CommandList list = kit.list(QueueType::Graphics, "test.fill");
    list.fillBuffer(dedicated, 4, 300, 0xA1B2C3D4u);
    list.fillBuffer(placed, 512, 260, 0x00FF10EFu);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(dedicated) == filled(background, 4, 300, 0xA1B2C3D4u));
    CHECK(kit.readBuffer(placed) == filled(background, 512, 260, 0x00FF10EFu));
}

//======================================================================================================================
TEST_CASE("two unequal fills in one encoder keep their own patterns and ranges", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> background = test::pattern(1024, 3);
    const Buffer first = kit.buffer(1024, Memory::Gpu, BufferUsage::ShaderWrite, background);
    const Buffer second = kit.buffer(1024, Memory::Gpu, BufferUsage::ShaderWrite, background);
    CommandList list = kit.list(QueueType::Graphics, "test.fill");
    // Both fills run on the one compute encoder the first opens, each with the kernel table's bindings as they stood at its
    // dispatch.
    list.fillBuffer(first, 0, 128, 0x01020304u);
    list.fillBuffer(first, 512, 256, 0x05060708u);
    list.fillBuffer(second, 256, 512, 0x090A0B0Cu);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(first) == filled(filled(background, 0, 128, 0x01020304u), 512, 256, 0x05060708u));
    CHECK(kit.readBuffer(second) == filled(background, 256, 512, 0x090A0B0Cu));
}

//======================================================================================================================
TEST_CASE("fills of one word and of a range not a multiple of 64 words", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> background = test::pattern(1024, 4);
    const Buffer buffer = kit.buffer(1024, Memory::Gpu, BufferUsage::ShaderWrite, background);
    CommandList list = kit.list(QueueType::Graphics, "test.fill");
    list.fillBuffer(buffer, 8, 4, 0xCAFEF00Du);
    // 100 words: the last threadgroup of 64 threads has 28 threads past the range, which write nothing.
    list.fillBuffer(buffer, 400, 400, 0x13579BDFu);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(buffer) == filled(filled(background, 8, 4, 0xCAFEF00Du), 400, 400, 0x13579BDFu));
}

//======================================================================================================================
TEST_CASE("fills record on Compute lists as on Graphics lists", "[conformance]")
{
    test::requireGpu();
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const std::vector<std::byte> background = test::pattern(512, 5);
    const Buffer buffer = kit.buffer(512, Memory::Gpu, BufferUsage::ShaderWrite, background);
    CommandList list = kit.list(type, "test.fill");
    list.fillBuffer(buffer, 0, 128, 0x42424242u);
    list.fillBuffer(buffer, 256, 192, 0x89ABCDEFu);
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(buffer, type) == filled(filled(background, 0, 128, 0x42424242u), 256, 192, 0x89ABCDEFu));
}

//======================================================================================================================
TEST_CASE("a barrier orders a copy before an unequal fill over the same range", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, test::pattern(kBig, 6));
    const Buffer target = kit.buffer(kBig, Memory::Gpu, BufferUsage::ShaderWrite);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.beginScope("test.copy");
    // The leading barrier gives the encoder an opening barrier, so the barrier below can stay inside it.
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(slow, 0, target, 0, kBig);
    list.barrier(std::array{ copyToCopy() });
    SECTION("the fill in a later encoder")
    {
        list.endScope();
        list.beginScope("test.fill");
    }
    SECTION("the fill in the same encoder")
    {
    }
    list.fillBuffer(target, kBig - kTail, kTail, 0xA5B6C7D8u);
    list.endScope();
    kit.run(std::array{ list });
    CHECK(readTail(kit, target) == filled(std::vector<std::byte>(kTail), 0, kTail, 0xA5B6C7D8u));
}

//======================================================================================================================
TEST_CASE("a barrier orders an unequal fill before a copy over the same range", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> tail = test::pattern(kTail, 7);
    const Buffer source = kit.buffer(kTail, Memory::Upload, BufferUsage::None, tail);
    const Buffer target = kit.buffer(kBig, Memory::Gpu, BufferUsage::ShaderWrite);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.beginScope("test.fill");
    // The leading barrier gives the encoder an opening barrier, so the barrier below can stay inside it.
    list.barrier(std::array{ copyToCopy() });
    list.fillBuffer(target, 0, kBig, 0x0F1E2D3Cu);
    list.barrier(std::array{ copyToCopy() });
    SECTION("the copy in a later encoder")
    {
        list.endScope();
        list.beginScope("test.copy");
    }
    SECTION("the copy in the same encoder")
    {
    }
    list.copyBuffer(source, 0, target, kBig - kTail, kTail);
    list.endScope();
    kit.run(std::array{ list });
    CHECK(readTail(kit, target) == tail);
}

//======================================================================================================================
TEST_CASE("a barrier orders a kernel fill before a copy that reads its result", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Buffer target = kit.buffer(kBig, Memory::Gpu, BufferUsage::ShaderWrite, test::pattern(kBig, 8));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.beginScope("test.fill");
    // The leading barrier gives the encoder an opening barrier, so the barrier below can stay inside it.
    list.barrier(std::array{ copyToCopy() });
    list.fillBuffer(target, 0, kBig, 0x1A2B3C4Du);
    list.barrier(std::array{ copyToCopy() });
    SECTION("the read in a later encoder")
    {
        list.endScope();
        list.beginScope("test.read");
    }
    SECTION("the read in the same encoder")
    {
    }
    list.copyBuffer(target, kBig - kTail, readback, 0, kTail);
    list.endScope();
    kit.run(std::array{ list });
    CHECK(kit.read(readback) == filled(std::vector<std::byte>(kTail), 0, kTail, 0x1A2B3C4Du));
}

//======================================================================================================================
TEST_CASE("a barrier after a kernel fill holds back every later encoder on the queue, not only the first", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Buffer target = kit.buffer(kBig, Memory::Gpu, BufferUsage::ShaderWrite);
    const std::vector<std::byte> unrelated = test::pattern(kTail, 9);
    const Buffer small = kit.buffer(kTail, Memory::Upload, BufferUsage::None, unrelated);
    const Buffer aside = kit.buffer(kTail, Memory::Gpu);
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    const BufferBarrier filledToRead{ .buffer = target,
                                      .offset = kBig - kTail,
                                      .size = kTail,
                                      .srcStage = Stage::Copy,
                                      .srcAccess = Access::CopyWrite,
                                      .dstStage = Stage::Copy,
                                      .dstAccess = Access::CopyRead };
    // Each round fills a fresh unequal pattern over what the round before left: the first later encoder copies other bytes,
    // the second reads the fill's last bytes.
    constexpr uint32_t kRounds = 4;
    const auto patternOf = [](uint32_t round) { return 0x10203040u + round * 0x01010101u + 1; };
    const auto expected = [&](uint32_t round) { return filled(std::vector<std::byte>(kTail), 0, kTail, patternOf(round)); };
    SECTION("within one Compute list, encoders split by scope boundaries")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList list = kit.list(QueueType::Compute, "test.list");
            list.fillBuffer(target, 0, kBig, patternOf(round));
            list.barrier({}, std::array{ filledToRead });
            list.beginScope("test.first");
            list.copyBuffer(small, 0, aside, 0, kTail);
            list.endScope();
            list.beginScope("test.second");
            list.copyBuffer(target, kBig - kTail, readback, 0, kTail);
            list.endScope();
            kit.run(std::array{ list });
            CHECK(kit.read(readback) == expected(round));
        }
    }
    SECTION("across two lists, the barrier at the end of the first")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList producer = kit.list(QueueType::Compute, "test.producer");
            producer.fillBuffer(target, 0, kBig, patternOf(round));
            producer.barrier({}, std::array{ filledToRead });
            CommandList consumer = kit.list(QueueType::Compute, "test.consumer");
            consumer.copyBuffer(small, 0, aside, 0, kTail);
            consumer.beginScope("test.second");
            consumer.copyBuffer(target, kBig - kTail, readback, 0, kTail);
            consumer.endScope();
            kit.run(std::array{ producer, consumer });
            CHECK(kit.read(readback) == expected(round));
        }
    }
    CHECK(kit.readBuffer(aside, QueueType::Compute) == unrelated);
}
