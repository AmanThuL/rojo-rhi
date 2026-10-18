//----------------------------------------------------------------------------------------------------------------------
/// @file BarrierConformanceTests.cpp
/// @brief Observes the barriers of chapter 09 on a GPU: hazards whose wrong order leaves different bytes, within and across encoders and lists.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The size of a slow copy: long enough that a later copy left unordered would overtake it.
    constexpr uint64_t kBig = uint64_t{ 32 } << 20;

    /// The bytes each case reads back from the end of a slow copy's destination.
    constexpr uint64_t kTail = 256;

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }

    //==================================================================================================================
    /// A barrier from copy writes of the last `kTail` bytes of a `kBig`-byte `buffer` to copy reads of them.
    BufferBarrier tailWrittenToRead(Buffer buffer)
    {
        return BufferBarrier{ .buffer = buffer,
                              .offset = kBig - kTail,
                              .size = kTail,
                              .srcStage = Stage::Copy,
                              .srcAccess = Access::CopyWrite,
                              .dstStage = Stage::Copy,
                              .dstAccess = Access::CopyRead };
    }

    //==================================================================================================================
    /// The last `kTail` bytes of `bytes`.
    std::vector<std::byte> tailOf(std::span<const std::byte> bytes)
    {
        return { bytes.end() - kTail, bytes.end() };
    }

    //==================================================================================================================
    /// Returns `size` bytes of `source` from `offset`, copied into a new Readback buffer by a list of `type` of its own.
    std::vector<std::byte> readRange(test::GpuKit& kit, Buffer source, uint64_t offset, uint64_t size, QueueType type = QueueType::Graphics)
    {
        const Buffer readback = kit.buffer(size, Memory::Readback);
        CommandList list = kit.list(type, "test.readRange");
        list.copyBuffer(source, offset, readback, 0, size);
        kit.run(std::array{ list });
        return kit.read(readback);
    }

    //==================================================================================================================
    /// Writes `pattern(kBig, seed)` into the Upload buffer `upload` through its CPU pointer and returns its last `kTail` bytes.
    std::vector<std::byte> writeSlowSource(test::GpuKit& kit, Buffer upload, uint32_t seed)
    {
        const std::vector<std::byte> bytes = test::pattern(kBig, seed);
        std::byte* pointer = kit.device.cpuPointer(upload);
        REQUIRE(pointer != nullptr);
        std::memcpy(pointer, bytes.data(), bytes.size());
        return tailOf(bytes);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a global barrier orders two copies into one buffer within a list", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> first = test::pattern(kBig, 1);
    const std::vector<std::byte> second = test::pattern(kTail, 2);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, first);
    const Buffer fast = kit.buffer(kTail, Memory::Upload, BufferUsage::None, second);
    const Buffer target = kit.buffer(kBig, Memory::Gpu);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.copyBuffer(slow, 0, target, 0, kBig);
    list.barrier(std::array{ copyToCopy() });
    list.copyBuffer(fast, 0, target, kBig - kTail, kTail);
    kit.run(std::array{ list });
    // The second write lands last: its bytes, not the first copy's, end the buffer.
    CHECK(readRange(kit, target, kBig - kTail, kTail) == second);
    CHECK(readRange(kit, target, 0, kTail) == std::vector<std::byte>(first.begin(), first.begin() + kTail));
}

//======================================================================================================================
TEST_CASE("a buffer barrier orders a copy before a copy that reads its result", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(kBig, 3);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, source);
    const Buffer middle = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 4));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.copyBuffer(slow, 0, middle, 0, kBig);
    list.barrier({}, std::array{ tailWrittenToRead(middle) });
    list.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
    kit.run(std::array{ list });
    CHECK(kit.read(readback) == tailOf(source));
}

//======================================================================================================================
TEST_CASE("a texture barrier orders a buffer-to-texture copy before a texture-to-buffer copy", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // The texture's bytes arrive through a slow copy into a staging buffer, so an unordered read finds the old ones.
    constexpr uint32_t kEdge = 2048;
    constexpr uint32_t kRow = kEdge * 4;
    constexpr uint32_t kRows = 4;
    const std::vector<std::byte> initial = test::pattern(uint64_t{ kRow } * kEdge, 5);
    const std::vector<std::byte> source = test::pattern(uint64_t{ kRow } * kEdge, 6);
    const std::array<TextureData, 1> data{ TextureData{ initial, kRow, 0 } };
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }, data
    );
    const Buffer upload = kit.buffer(source.size(), Memory::Upload, BufferUsage::None, source);
    const Buffer staging = kit.buffer(source.size(), Memory::Gpu);
    const Buffer readback = kit.buffer(uint64_t{ kRow } * kRows, Memory::Readback);
    const BufferTextureLayout layout{ .rowPitch = kRow };
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.copyBuffer(upload, 0, staging, 0, source.size());
    // A texture made with initial data starts in ShaderRead; nothing on this queue used it, so the move orders no work on
    // the texture.
    list.barrier(
        {},
        std::array{ BufferBarrier{
            .buffer = staging, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } },
        std::array{ TextureBarrier{
            .texture = texture, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite, .oldLayout = Layout::ShaderRead, .newLayout = Layout::CopyDst } }
    );
    list.copyBufferToTexture(staging, layout, texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } });
    const TextureBarrier writtenToRead{ .texture = texture,
                                        .srcStage = Stage::Copy,
                                        .srcAccess = Access::CopyWrite,
                                        .dstStage = Stage::Copy,
                                        .dstAccess = Access::CopyRead,
                                        .oldLayout = Layout::CopyDst,
                                        .newLayout = Layout::CopySrc };
    const TextureRegion lastRows{ .offset = { 0, kEdge - kRows, 0 }, .extent = { kEdge, kRows, 1 } };
    SECTION("within one list")
    {
        // On the Apple GPUs tried, texture blits within one encoder ran in order even without the barrier, so this section
        // does not fail when the barrier is dropped; the next one does.
        list.barrier({}, {}, std::array{ writtenToRead });
        list.copyTextureToBuffer(texture, lastRows, readback, layout);
        kit.run(std::array{ list });
    }
    SECTION("the read in the next list")
    {
        CommandList reader = kit.list(QueueType::Graphics, "test.reader");
        reader.barrier({}, {}, std::array{ writtenToRead });
        reader.copyTextureToBuffer(texture, lastRows, readback, layout);
        kit.run(std::array{ list, reader });
    }
    CHECK(kit.read(readback) == std::vector<std::byte>(source.end() - uint64_t{ kRow } * kRows, source.end()));
}

//======================================================================================================================
TEST_CASE("a barrier at the start of a list orders the previous list's copy on the queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(kBig, 7);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, source);
    const Buffer middle = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 8));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    producer.copyBuffer(slow, 0, middle, 0, kBig);
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    consumer.barrier({}, std::array{ tailWrittenToRead(middle) });
    consumer.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
    // Both lists go in one submission, so only the barrier orders them.
    kit.run(std::array{ producer, consumer });
    CHECK(kit.read(readback) == tailOf(source));
}

//======================================================================================================================
TEST_CASE("a barrier at the end of a list orders the next list's copy on the queue", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(kBig, 9);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, source);
    const Buffer middle = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 10));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    CommandList producer = kit.list(QueueType::Graphics, "test.producer");
    producer.copyBuffer(slow, 0, middle, 0, kBig);
    producer.barrier({}, std::array{ tailWrittenToRead(middle) });
    CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
    consumer.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
    kit.run(std::array{ producer, consumer });
    CHECK(kit.read(readback) == tailOf(source));
}

//======================================================================================================================
TEST_CASE("barriers across a scope boundary order copies in two encoders", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(kBig, 11);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, source);
    const Buffer middle = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 12));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    list.beginScope("test.produce");
    list.copyBuffer(slow, 0, middle, 0, kBig);
    SECTION("the barrier between the scopes")
    {
        list.endScope();
        list.barrier({}, std::array{ tailWrittenToRead(middle) });
    }
    SECTION("the barrier inside the producing scope")
    {
        list.barrier({}, std::array{ tailWrittenToRead(middle) });
        list.endScope();
    }
    list.beginScope("test.consume");
    list.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
    list.endScope();
    kit.run(std::array{ list });
    CHECK(kit.read(readback) == tailOf(source));
}

//======================================================================================================================
TEST_CASE("a chain of 64 copies with a barrier between each ends with the first source's bytes", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    constexpr uint64_t kSize = uint64_t{ 1 } << 20;
    constexpr uint32_t kCopies = 64;
    const std::vector<std::byte> source = test::pattern(kSize, 13);
    std::vector<Buffer> buffers{ kit.buffer(kSize, Memory::Upload, BufferUsage::None, source) };
    for (uint32_t index = 0; index < kCopies; ++index)
    {
        buffers.push_back(kit.buffer(kSize, Memory::Gpu, BufferUsage::None, test::pattern(kSize, 14 + index)));
    }
    CommandList list = kit.list(QueueType::Graphics, "test.list");
    for (uint32_t index = 0; index < kCopies; ++index)
    {
        if (index > 0)
        {
            list.barrier(
                {},
                std::array{ BufferBarrier{ .buffer = buffers[index],
                                           .srcStage = Stage::Copy,
                                           .srcAccess = Access::CopyWrite,
                                           .dstStage = Stage::Copy,
                                           .dstAccess = Access::CopyRead } }
            );
        }
        list.copyBuffer(buffers[index], 0, buffers[index + 1], 0, kSize);
    }
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(buffers.back()) == source);
}

//======================================================================================================================
TEST_CASE("barriers order copies on the Compute and Copy queues as on Graphics", "[conformance]")
{
    test::requireGpu();
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(kBig, 15);
    const Buffer slow = kit.buffer(kBig, Memory::Upload, BufferUsage::None, source);
    const Buffer middle = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 16));
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    SECTION("within a list")
    {
        CommandList list = kit.list(type, "test.list");
        list.copyBuffer(slow, 0, middle, 0, kBig);
        list.barrier({}, std::array{ tailWrittenToRead(middle) });
        list.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
        kit.run(std::array{ list });
    }
    SECTION("across lists")
    {
        CommandList producer = kit.list(type, "test.producer");
        producer.copyBuffer(slow, 0, middle, 0, kBig);
        producer.barrier({}, std::array{ tailWrittenToRead(middle) });
        CommandList consumer = kit.list(type, "test.consumer");
        consumer.copyBuffer(middle, kBig - kTail, readback, 0, kTail);
        kit.run(std::array{ producer, consumer });
    }
    CHECK(kit.read(readback) == tailOf(source));
}

//======================================================================================================================
TEST_CASE("a barrier holds back every later encoder on the queue, not only the first", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Buffer slow = kit.buffer(kBig, Memory::Upload);
    const Buffer target = kit.buffer(kBig, Memory::Gpu, BufferUsage::None, test::pattern(kBig, 17));
    const std::vector<std::byte> unrelated = test::pattern(kTail, 18);
    const Buffer small = kit.buffer(kTail, Memory::Upload, BufferUsage::None, unrelated);
    const Buffer aside = kit.buffer(kTail, Memory::Gpu);
    const Buffer readback = kit.buffer(kTail, Memory::Readback);
    // Each round writes a fresh pattern over the bytes the round before left in the target.
    constexpr uint32_t kRounds = 4;
    SECTION("within one Compute list, encoders split by scope boundaries")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            const std::vector<std::byte> expected = writeSlowSource(kit, slow, 19 + round);
            CommandList list = kit.list(QueueType::Compute, "test.list");
            list.copyBuffer(slow, 0, target, 0, kBig);
            list.barrier({}, std::array{ tailWrittenToRead(target) });
            list.beginScope("test.first");
            list.copyBuffer(small, 0, aside, 0, kTail);
            list.endScope();
            list.beginScope("test.second");
            list.copyBuffer(target, kBig - kTail, readback, 0, kTail);
            list.endScope();
            kit.run(std::array{ list });
            CHECK(kit.read(readback) == expected);
        }
    }
    SECTION("across two lists, the barrier at the end of the first")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            const std::vector<std::byte> expected = writeSlowSource(kit, slow, 29 + round);
            CommandList producer = kit.list(QueueType::Compute, "test.producer");
            producer.copyBuffer(slow, 0, target, 0, kBig);
            producer.barrier({}, std::array{ tailWrittenToRead(target) });
            CommandList consumer = kit.list(QueueType::Compute, "test.consumer");
            consumer.copyBuffer(small, 0, aside, 0, kTail);
            consumer.beginScope("test.second");
            consumer.copyBuffer(target, kBig - kTail, readback, 0, kTail);
            consumer.endScope();
            kit.run(std::array{ producer, consumer });
            CHECK(kit.read(readback) == expected);
        }
    }
    SECTION("across two lists, the barrier at the start of the second")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            const std::vector<std::byte> expected = writeSlowSource(kit, slow, 39 + round);
            CommandList producer = kit.list(QueueType::Compute, "test.producer");
            producer.copyBuffer(slow, 0, target, 0, kBig);
            CommandList consumer = kit.list(QueueType::Compute, "test.consumer");
            consumer.barrier({}, std::array{ tailWrittenToRead(target) });
            consumer.copyBuffer(small, 0, aside, 0, kTail);
            consumer.beginScope("test.second");
            consumer.copyBuffer(target, kBig - kTail, readback, 0, kTail);
            consumer.endScope();
            kit.run(std::array{ producer, consumer });
            CHECK(kit.read(readback) == expected);
        }
    }
    CHECK(readRange(kit, aside, 0, kTail) == unrelated);
}
