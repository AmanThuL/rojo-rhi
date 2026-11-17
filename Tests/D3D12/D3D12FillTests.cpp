//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12FillTests.cpp
/// @brief Tests D3D12 buffer fills against the fill kernel's pipeline, the calls they record and the bytes they write.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12Kernels.h"
#include "D3D12ListState.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    struct TestRoot
    {
        uint32_t values[4] = {};
    };

    // The message callback may fill `g_messages` from any thread.
    std::mutex g_messagesMutex;

    std::vector<std::string> g_messages;

    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    // Collects the library's warnings and errors, the debug layer's included, into `g_messages` (emptied first) while it
    // lives, and removes the callback however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        MessageCollection(const MessageCollection&) = delete;

        MessageCollection& operator=(const MessageCollection&) = delete;

        // Also lists the messages for the log.
        size_t count() const;
    };

    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    size_t MessageCollection::count() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages.size();
    }

    Device makeDevice()
    {
        Result<Device> created = createDevice({});
        REQUIRE(created.has_value());
        return *created;
    }

    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    Buffer makeWritable(Device device, uint64_t size, std::string_view label)
    {
        Result<Buffer> created = device.createBuffer({ .size = size, .usage = BufferUsage::ShaderWrite, .label = label });
        REQUIRE(created.has_value());
        return *created;
    }

    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    d3d12::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    std::wstring objectName(ID3D12Object* object)
    {
        wchar_t name[128] = {};
        UINT size = static_cast<UINT>(sizeof(name));
        if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            return {};
        }
        // The size counts bytes; a terminating null, when stored, is dropped.
        std::wstring read(name, size / sizeof(wchar_t));
        while (!read.empty() && read.back() == L'\0')
        {
            read.pop_back();
        }
        return read;
    }

    // Ends the frame and waits, so no queue uses what the list wrote.
    void submitAndWait(Device device, CommandList list, Fence done, uint64_t value)
    {
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, value } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        device.endFrame();
        REQUIRE(device.wait({ done, value }, UINT64_MAX));
    }

    // Little-endian; `offset` and `size` are multiples of 4.
    void writeWords(std::vector<std::byte>& bytes, uint64_t offset, uint64_t size, uint32_t word)
    {
        for (uint64_t at = offset; at < offset + size; ++at)
        {
            bytes[at] = static_cast<std::byte>((word >> ((at % 4) * 8)) & 0xFFu);
        }
    }

    // Their size when they are equal.
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

TEST_CASE("the device creates the fill pipeline", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12PipelineState* fill = DeviceImpl::of(device).kernels.fill();
    REQUIRE(fill != nullptr);
    CHECK(objectName(fill) == L"rojorhi.kernel.fill");
    destroyDevice(device);
}

TEST_CASE("a fill records the kernel's root signature, pipeline, root view, constants and dispatch", "[d3d12]")
{
    constexpr uint64_t kSize = 256;
    constexpr uint32_t kPattern = 0xA1B2C3D4u;
    Device device = makeDevice();
    const MessageCollection messages;
    const Buffer buffer = makeWritable(device, kSize, "test.fill");
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const uint64_t before = recordCalls(list);
    const uint64_t writes = device.counters().descriptorWrites;
    list.fillBuffer(buffer, 0, kSize, kPattern);
    CHECK(recordCalls(list) - before == 5u);
    // The window is a root view at the buffer's address, so no descriptor is written.
    CHECK(device.counters().descriptorWrites == writes);
    submitAndWait(device, list, done, 1);
    std::vector<std::byte> expected(kSize);
    writeWords(expected, 0, kSize, kPattern);
    CHECK(test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, buffer), kSize) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(buffer);
    destroyDevice(device);
}

TEST_CASE("a fill right after a fill records only its root view, constants and dispatch", "[d3d12]")
{
    constexpr uint64_t kHalf = 256;
    Device device = makeDevice();
    const MessageCollection messages;
    const Buffer buffer = makeWritable(device, kHalf * 2, "test.fill");
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    // The two fills write disjoint halves, so they need no barrier between them.
    list.fillBuffer(buffer, 0, kHalf, 0x01020304u);
    const uint64_t before = recordCalls(list);
    list.fillBuffer(buffer, kHalf, kHalf, 0x0A0B0C0Du);
    CHECK(recordCalls(list) - before == 3u);
    submitAndWait(device, list, done, 1);
    std::vector<std::byte> expected(kHalf * 2);
    writeWords(expected, 0, kHalf, 0x01020304u);
    writeWords(expected, kHalf, kHalf, 0x0A0B0C0Du);
    CHECK(test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, buffer), kHalf * 2) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(buffer);
    destroyDevice(device);
}

TEST_CASE("a fill marks the compute root dirty and leaves the graphics side alone", "[d3d12]")
{
    constexpr uint64_t kSize = 64;
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const Buffer first = makeWritable(device, kSize, "test.first");
    const Buffer second = makeWritable(device, kSize, "test.second");
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const d3d12::Binding& binding = stateOf(list).binding;
    REQUIRE_FALSE(binding.computeRootDirty);
    REQUIRE_FALSE(binding.graphicsRootDirty);
    list.fillBuffer(first, 0, kSize, 0x11223344u);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Kernel);
    CHECK(binding.computePipeline == impl.kernels.fill());
    CHECK(binding.computeRootDirty);
    CHECK_FALSE(binding.graphicsRootDirty);
    CHECK(binding.graphicsPipeline == nullptr);
    CHECK(binding.root == 0u);
    // A root set before a fill stays bound for the graphics side and is bound again on the compute side.
    list.pushRoot(TestRoot{ { 1, 2, 3, 4 } });
    const D3D12_GPU_VIRTUAL_ADDRESS root = binding.root;
    REQUIRE(root != 0u);
    list.fillBuffer(second, 0, kSize, 0x55667788u);
    CHECK(binding.root == root);
    CHECK(binding.computeRootDirty);
    CHECK(binding.graphicsRootDirty);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Kernel);
    submitAndWait(device, list, done, 1);
    std::vector<std::byte> expected(kSize);
    writeWords(expected, 0, kSize, 0x11223344u);
    CHECK(test::readNativeBuffer(impl, resourceOf(device, first), kSize) == expected);
    writeWords(expected, 0, kSize, 0x55667788u);
    CHECK(test::readNativeBuffer(impl, resourceOf(device, second), kSize) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(second);
    device.destroy(first);
    destroyDevice(device);
}

TEST_CASE("fills record on Compute lists as on Graphics lists", "[d3d12]")
{
    constexpr uint64_t kSize = 1024;
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const Buffer graphicsTarget = makeWritable(device, kSize, "test.graphicsTarget");
    const Buffer computeTarget = makeWritable(device, kSize, "test.computeTarget");
    const Fence computeDone = makeFence(device, "test.computeDone");
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    const std::array<CommandList, 2> lists{ graphics, compute };
    const std::array<Buffer, 2> targets{ graphicsTarget, computeTarget };
    for (size_t i = 0; i < lists.size(); ++i)
    {
        INFO((i == 0 ? "Graphics" : "Compute") << " list");
        CommandList list = lists[i];
        const Buffer target = targets[i];
        const uint64_t before = recordCalls(list);
        list.fillBuffer(target, 0, kSize / 2, 0xDEADBEEFu);
        CHECK(recordCalls(list) - before == 5u);
        list.fillBuffer(target, kSize / 2, kSize / 2, 0x0BADF00Du);
        CHECK(recordCalls(list) - before == 8u);
    }
    // The compute list signals a fence the graphics submission waits for, so the frame joins it.
    compute.end();
    const std::array<FenceValue, 1> computeSignal{ FenceValue{ computeDone, 1 } };
    device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, computeSignal);
    graphics.end();
    const std::array<FenceValue, 1> join{ FenceValue{ computeDone, 1 } };
    const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), join, signal);
    device.endFrame();
    REQUIRE(device.wait({ computeDone, 1 }, UINT64_MAX));
    REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
    std::vector<std::byte> expected(kSize);
    writeWords(expected, 0, kSize / 2, 0xDEADBEEFu);
    writeWords(expected, kSize / 2, kSize / 2, 0x0BADF00Du);
    CHECK(test::readNativeBuffer(impl, resourceOf(device, graphicsTarget), kSize) == expected);
    CHECK(test::readNativeBuffer(impl, resourceOf(device, computeTarget), kSize) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(computeDone);
    device.destroy(computeTarget);
    device.destroy(graphicsTarget);
    destroyDevice(device);
}

TEST_CASE("a fill past 2 GiB dispatches once per window, writes no descriptor and nothing past its end", "[d3d12]")
{
    constexpr uint64_t kWindow = d3d12::kFillWindowBytes;
    constexpr uint64_t kSize = kWindow + (uint64_t{ 1 } << 20);
    // The second fill starts 64 KiB into the buffer and ends 256 KiB into the second window, so its last window ends mid-way.
    constexpr uint64_t kStart = uint64_t{ 1 } << 16;
    constexpr uint64_t kEnd = kWindow + (uint64_t{ 1 } << 18);
    constexpr uint64_t kTail = 512;
    Device device = makeDevice();
    const MessageCollection messages;
    Result<Buffer> created = device.createBuffer({ .size = kSize, .usage = BufferUsage::ShaderWrite, .label = "test.large" });
    if (!created && created.error().category == ErrorCategory::OutOfMemory)
    {
        destroyDevice(device);
        SKIP("the adapter cannot hold a buffer of 2 GiB and 1 MiB: " << created.error().message);
    }
    REQUIRE(created.has_value());
    const Buffer buffer = *created;
    const Buffer readback = *device.createBuffer({ .size = kTail, .memory = Memory::Readback, .label = "test.tail" });
    const Buffer seam = *device.createBuffer({ .size = kTail, .memory = Memory::Readback, .label = "test.seam" });
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const uint64_t writes = device.counters().descriptorWrites;
    // The whole buffer: the kernel binds, then each of two windows sets its root view and constants and dispatches.
    const uint64_t calls = recordCalls(list);
    list.fillBuffer(buffer, 0, kSize, 0x11111111u);
    CHECK(recordCalls(list) - calls == 8u);
    const std::array<GlobalBarrier, 1> between = { GlobalBarrier{
        .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite } };
    list.barrier(between, {}, {});
    const uint64_t laterCalls = recordCalls(list);
    list.fillBuffer(buffer, kStart, kEnd - kStart, 0x2468ACE0u);
    CHECK(recordCalls(list) - laterCalls == 6u);
    CHECK(device.counters().descriptorWrites == writes);
    list.barrier(between, {}, {});
    list.copyBuffer(buffer, kEnd - kTail / 2, readback, 0, kTail);
    // The second fill's first window ends 2 GiB after its start, where its second begins.
    list.copyBuffer(buffer, kStart + kWindow - kTail / 2, seam, 0, kTail);
    submitAndWait(device, list, done, 1);
    // The last 256 bytes the second fill wrote hold its pattern; the 256 after its end keep the first fill's.
    const std::byte* tail = device.cpuPointer(readback);
    REQUIRE(tail != nullptr);
    std::vector<std::byte> expected(kTail);
    writeWords(expected, 0, kTail / 2, 0x2468ACE0u);
    writeWords(expected, kTail / 2, kTail / 2, 0x11111111u);
    CHECK(std::vector<std::byte>(tail, tail + kTail) == expected);
    // Across the seam between its two windows the second fill's pattern runs unbroken.
    const std::byte* across = device.cpuPointer(seam);
    REQUIRE(across != nullptr);
    std::vector<std::byte> unbroken(kTail);
    writeWords(unbroken, 0, kTail, 0x2468ACE0u);
    CHECK(std::vector<std::byte>(across, across + kTail) == unbroken);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(readback);
    device.destroy(seam);
    device.destroy(buffer);
    destroyDevice(device);
}

TEST_CASE("fills of one word and of 2^22 + 3 words dispatch the group counts the kernel loops over", "[d3d12]")
{
    constexpr uint64_t kLongWords = (uint64_t{ 1 } << 22) + 3;
    constexpr uint64_t kMargin = 16;
    constexpr uint64_t kShortSize = 64;
    constexpr uint64_t kShortOffset = 28;
    constexpr uint64_t kLongSize = kMargin + kLongWords * 4 + kMargin;
    constexpr uint32_t kEarlier = 0x11111111u;
    constexpr uint32_t kPattern = 0xCAFEF00Du;
    // One group for one word; the longest dispatch for 2^22 + 3 words, whose threads loop over the words past it.
    CHECK(d3d12::fillGroups(1) == 1u);
    CHECK(d3d12::fillGroups(64) == 1u);
    CHECK(d3d12::fillGroups(65) == 2u);
    CHECK(d3d12::fillGroups(kLongWords) == 65'535u);
    STATIC_REQUIRE(uint64_t{ 65'535 } * d3d12::kFillGroupThreads < kLongWords);
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const Buffer shortBuffer = makeWritable(device, kShortSize, "test.short");
    const Buffer longBuffer = makeWritable(device, kLongSize, "test.long");
    const Fence done = makeFence(device, "test.done");
    // The earlier fill covers both buffers and finishes before the later one starts.
    device.beginFrame();
    CommandList earlier = device.acquireCommandList(QueueType::Graphics, "test.earlier");
    earlier.fillBuffer(shortBuffer, 0, kShortSize, kEarlier);
    earlier.fillBuffer(longBuffer, 0, kLongSize, kEarlier);
    submitAndWait(device, earlier, done, 1);
    device.beginFrame();
    CommandList later = device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(shortBuffer, kShortOffset, 4, kPattern);
    later.fillBuffer(longBuffer, kMargin, kLongWords * 4, kPattern);
    submitAndWait(device, later, done, 2);
    std::vector<std::byte> expected(kShortSize);
    writeWords(expected, 0, kShortSize, kEarlier);
    writeWords(expected, kShortOffset, 4, kPattern);
    CHECK(firstDifference(test::readNativeBuffer(impl, resourceOf(device, shortBuffer), kShortSize), expected) == kShortSize);
    expected.assign(kLongSize, std::byte{ 0 });
    writeWords(expected, 0, kLongSize, kEarlier);
    writeWords(expected, kMargin, kLongWords * 4, kPattern);
    CHECK(firstDifference(test::readNativeBuffer(impl, resourceOf(device, longBuffer), kLongSize), expected) == kLongSize);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(longBuffer);
    device.destroy(shortBuffer);
    destroyDevice(device);
}

TEST_CASE("an unequal pattern lands in little-endian order over exactly the range", "[d3d12]")
{
    constexpr uint64_t kSize = 64;
    Device device = makeDevice();
    const MessageCollection messages;
    const Buffer buffer = makeWritable(device, kSize, "test.fill");
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList earlier = device.acquireCommandList(QueueType::Graphics, "test.earlier");
    // Four equal bytes take the same kernel as four different ones.
    earlier.fillBuffer(buffer, 0, kSize, 0xEEEEEEEEu);
    submitAndWait(device, earlier, done, 1);
    device.beginFrame();
    CommandList later = device.acquireCommandList(QueueType::Graphics, "test.later");
    later.fillBuffer(buffer, 8, 16, 0x04030201u);
    submitAndWait(device, later, done, 2);
    std::vector<std::byte> expected(kSize, std::byte{ 0xEE });
    for (uint64_t at = 8; at < 24; ++at)
    {
        expected[at] = static_cast<std::byte>(at % 4 + 1);
    }
    CHECK(test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, buffer), kSize) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(buffer);
    destroyDevice(device);
}
