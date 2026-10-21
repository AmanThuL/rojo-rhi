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
    /// A root block a case pushes: one 16-byte row.
    struct TestRoot
    {
        uint32_t values[4] = {};
    };

    /// Guards `g_messages`, which the message callback may fill from any thread.
    std::mutex g_messagesMutex;

    /// The messages at `Warning` or above the library reported while a case collected them.
    std::vector<std::string> g_messages;

    //==================================================================================================================
    /// Keeps `message` when it is a warning or an error. It may run on any thread.
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    /// Collects the library's warnings and errors, the debug layer's included, into `g_messages`, emptied first, while it
    /// lives, and removes the callback when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;

        /// Returns the messages collected so far, and lists them for the log.
        size_t count() const;
    };

    //==================================================================================================================
    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    //==================================================================================================================
    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    size_t MessageCollection::count() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages.size();
    }

    //==================================================================================================================
    /// Returns a new device with the default frames in flight.
    Device makeDevice()
    {
        Result<Device> created = createDevice({});
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new fence at zero on `device`, labelled `label`.
    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new `Gpu` buffer of `size` bytes with `ShaderWrite` on `device`, labelled `label`.
    Buffer makeWritable(Device device, uint64_t size, std::string_view label)
    {
        Result<Buffer> created = device.createBuffer({ .size = size, .usage = BufferUsage::ShaderWrite, .label = label });
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns the native buffer of `buffer`, a live buffer of `device`.
    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native state of `list`.
    d3d12::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Returns the name `object` carries, without a terminating null.
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

    //==================================================================================================================
    /// Ends `list`, a Graphics list of `device`'s open frame, submits it signalling `value` of `done`, ends the frame and
    /// waits for the signal, so no queue uses what the list wrote.
    void submitAndWait(Device device, CommandList list, Fence done, uint64_t value)
    {
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, value } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        device.endFrame();
        REQUIRE(device.wait({ done, value }, UINT64_MAX));
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
TEST_CASE("the device creates the fill pipeline", "[d3d12]")
{
    Device device = makeDevice();
    ID3D12PipelineState* fill = DeviceImpl::of(device).kernels.fill();
    REQUIRE(fill != nullptr);
    CHECK(objectName(fill) == L"rojorhi.kernel.fill");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fill records the kernel's root signature, pipeline, constants and dispatch", "[d3d12]")
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
    list.fillBuffer(buffer, 0, kSize, kPattern);
    CHECK(recordCalls(list) - before == 4u);
    submitAndWait(device, list, done, 1);
    std::vector<std::byte> expected(kSize);
    writeWords(expected, 0, kSize, kPattern);
    CHECK(test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, buffer), kSize) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(buffer);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a fill right after a fill records only constants and dispatch", "[d3d12]")
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
    CHECK(recordCalls(list) - before == 2u);
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

//======================================================================================================================
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

//======================================================================================================================
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
        CHECK(recordCalls(list) - before == 4u);
        list.fillBuffer(target, kSize / 2, kSize / 2, 0x0BADF00Du);
        CHECK(recordCalls(list) - before == 6u);
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

//======================================================================================================================
TEST_CASE("a fill reaching past 512 MiB dispatches once per window and writes one transient view per later window", "[d3d12]")
{
    constexpr uint64_t kReach = uint64_t{ 1 } << 29;
    constexpr uint64_t kSize = kReach + (uint64_t{ 1 } << 16);
    constexpr uint64_t kOffset = kReach - (uint64_t{ 1 } << 15);
    constexpr uint64_t kFill = uint64_t{ 1 } << 16;
    Device device = makeDevice();
    const MessageCollection messages;
    Result<Buffer> created = device.createBuffer({ .size = kSize, .usage = BufferUsage::ShaderWrite, .label = "test.large" });
    if (!created && created.error().category == ErrorCategory::OutOfMemory)
    {
        destroyDevice(device);
        SKIP("the adapter cannot hold a buffer of 512 MiB and 64 KiB: " << created.error().message);
    }
    REQUIRE(created.has_value());
    const Buffer buffer = *created;
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const uint64_t calls = recordCalls(list);
    const uint64_t writes = device.counters().descriptorWrites;
    list.fillBuffer(buffer, kOffset, kFill, 0x13579BDFu);
    // The window below 512 MiB binds the kernel, sets its constants and dispatches; the window above sets constants and
    // dispatches through a transient raw view written for it.
    CHECK(recordCalls(list) - calls == 6u);
    CHECK(device.counters().descriptorWrites - writes == 1u);
    // A fill wholly above 512 MiB, starting inside a window, skips the buffer's own view: the kernel is still bound, so it
    // sets constants and dispatches through one transient view.
    const uint64_t laterCalls = recordCalls(list);
    const uint64_t laterWrites = device.counters().descriptorWrites;
    list.fillBuffer(buffer, kOffset + kFill, kSize - kOffset - kFill, 0x2468ACE0u);
    CHECK(recordCalls(list) - laterCalls == 2u);
    CHECK(device.counters().descriptorWrites - laterWrites == 1u);
    submitAndWait(device, list, done, 1);
    CHECK(messages.count() == 0u);
    device.destroy(done);
    device.destroy(buffer);
    destroyDevice(device);
}

//======================================================================================================================
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

//======================================================================================================================
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
