//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CommandListTests.cpp
/// @brief Tests D3D12 command lists against the native lists and allocators they record into, their submission and root.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The longest a case waits on a native fence before it fails: thirty seconds, in nanoseconds.
    constexpr uint64_t kNativeWaitNs = 30'000'000'000;

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
    /// Returns a new device that keeps `framesInFlight` frames in flight.
    Device makeDevice(uint32_t framesInFlight = 3)
    {
        DeviceDesc desc;
        desc.framesInFlight = framesInFlight;
        Result<Device> created = createDevice(desc);
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
    /// Returns a new buffer of `desc` on `device`, holding `initial` when it is not empty.
    Buffer makeBuffer(Device device, const BufferDesc& desc, std::span<const std::byte> initial = {})
    {
        Result<Buffer> created = device.createBuffer(desc, initial);
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
    /// Returns `ascii`, a label of ASCII characters, as the UTF-16 name a native object carries.
    std::wstring wide(std::string_view ascii)
    {
        std::wstring name;
        for (const char c : ascii)
        {
            name.push_back(static_cast<wchar_t>(c));
        }
        return name;
    }

    //==================================================================================================================
    /// Returns the native name of list `ordinal` of frame slot `slot`'s pool of `type`.
    std::wstring poolName(uint32_t slot, std::wstring_view type, uint32_t ordinal)
    {
        return L"rojorhi.list." + std::to_wstring(slot) + L"." + std::wstring(type) + L"." + std::to_wstring(ordinal);
    }

    //==================================================================================================================
    /// Returns the frame slot of `device`'s open frame.
    uint32_t openSlot(Device device)
    {
        const core::DeviceCore& core = DeviceImpl::of(device).core;
        return static_cast<uint32_t>(core.frameNumber() % core.framesInFlight());
    }

    //==================================================================================================================
    /// Returns `size` bytes counting up from `seed`, wrapping at 256.
    std::vector<std::byte> pattern(size_t size, uint32_t seed)
    {
        std::vector<std::byte> bytes(size);
        for (size_t i = 0; i < size; ++i)
        {
            bytes[i] = static_cast<std::byte>((i + seed) & 0xFF);
        }
        return bytes;
    }

    //==================================================================================================================
    /// Submits `graphics` and `compute` on their queues and `copy` on its queue, the compute and copy lists each signalling
    /// value `value` of their fence, which the graphics submission waits for, so the frame joins them.
    void submitJoined(Device device, CommandList graphics, CommandList compute, CommandList copy, Fence computeDone, Fence copyDone, uint64_t value)
    {
        const std::array<FenceValue, 1> computeSignal{ FenceValue{ computeDone, value } };
        device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, computeSignal);
        const std::array<FenceValue, 1> copySignal{ FenceValue{ copyDone, value } };
        device.queue(QueueType::Copy).submit(std::span(&copy, 1), {}, copySignal);
        const std::array<FenceValue, 2> joins{ FenceValue{ computeDone, value }, FenceValue{ copyDone, value } };
        device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), joins, {});
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Graphics list's acquisition resets it, sets both heaps and both root signatures", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    const uint32_t slot = openSlot(device);
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    // Reset, SetDescriptorHeaps, SetGraphicsRootSignature and SetComputeRootSignature.
    CHECK(recordCalls(list) == 4u);
    const d3d12::ListState& state = stateOf(list);
    REQUIRE(state.list);
    REQUIRE(state.allocator);
    CHECK(state.list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT);
    CHECK(state.type == QueueType::Graphics);
    CHECK(state.frameSlot == slot);
    CHECK(state.ordinal == 0u);
    CHECK(objectName(state.allocator.get()) == poolName(slot, L"graphics", 0));
    CHECK(objectName(state.list.get()) == L"test.graphics");
    CHECK(state.binding.root == 0u);
    CHECK(state.binding.computeRoot == d3d12::ComputeRoot::Global);
    CHECK(state.binding.drawIdDirty);
    list.end();
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    device.endFrame();
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Compute list sets the heaps and the compute root signature, a Copy list only resets", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence computeDone = makeFence(device, "test.computeDone");
    const Fence copyDone = makeFence(device, "test.copyDone");
    device.beginFrame();
    const uint32_t slot = openSlot(device);
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
    // Reset, SetDescriptorHeaps and SetComputeRootSignature; Reset alone.
    CHECK(recordCalls(compute) == 3u);
    CHECK(recordCalls(copy) == 1u);
    CHECK(stateOf(compute).list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    CHECK(stateOf(copy).list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY);
    CHECK(objectName(stateOf(compute).allocator.get()) == poolName(slot, L"compute", 0));
    CHECK(objectName(stateOf(copy).allocator.get()) == poolName(slot, L"copy", 0));
    graphics.end();
    compute.end();
    copy.end();
    submitJoined(device, graphics, compute, copy, computeDone, copyDone, 1);
    device.endFrame();
    CHECK(device.wait({ computeDone, 1 }, UINT64_MAX));
    CHECK(device.wait({ copyDone, 1 }, UINT64_MAX));
    CHECK(messages.count() == 0u);
    device.destroy(computeDone);
    device.destroy(copyDone);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("end closes the list with one native call", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const uint64_t before = recordCalls(list);
    list.end();
    CHECK(recordCalls(list) - before == 1u);
    // Executing a list that is not closed is a debug-layer error, so a clean submission shows that end closed it.
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    device.endFrame();
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission of three lists executes them in one call", "[d3d12]")
{
    constexpr uint64_t kSlice = 256;
    constexpr std::array<QueueType, 3> kTypes = { QueueType::Graphics, QueueType::Compute, QueueType::Copy };
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const std::vector<std::byte> bytes = pattern(kSlice * 3, 11);
    // Each queue copies from a source of its own, so no buffer is used on two queues at once.
    std::array<Buffer, 3> sources = {};
    std::array<std::array<Buffer, 3>, 3> targets = {};
    std::array<Fence, 3> done = {};
    for (size_t type = 0; type < kTypes.size(); ++type)
    {
        sources[type] = makeBuffer(device, { .size = kSlice * 3, .label = "test.source" }, bytes);
        done[type] = makeFence(device, "test.done");
        for (Buffer& target : targets[type])
        {
            target = makeBuffer(device, { .size = kSlice, .label = "test.target" });
        }
    }
    device.beginFrame();
    for (size_t type = 0; type < kTypes.size(); ++type)
    {
        INFO("queue type " << type);
        // Each list copies its own slice natively, so the bytes show that every list of the submission ran.
        std::array<CommandList, 3> lists = {};
        for (size_t i = 0; i < lists.size(); ++i)
        {
            lists[i] = device.acquireCommandList(kTypes[type], "test.list");
            stateOf(lists[i]).list->CopyBufferRegion(resourceOf(device, targets[type][i]), 0, resourceOf(device, sources[type]), i * kSlice, kSlice);
            lists[i].end();
        }
        const uint64_t before = device.counters().nativeSubmitCalls;
        const std::array<FenceValue, 1> signal{ FenceValue{ done[type], 1 } };
        device.queue(kTypes[type]).submit(lists, {}, signal);
        // The signal the core counts and one ExecuteCommandLists. A compute or copy submission with lists and one signal
        // joins the frame, so its queue signals no progress.
        CHECK(device.counters().nativeSubmitCalls - before == 2u);
    }
    const std::array<FenceValue, 2> joins{ FenceValue{ done[1], 1 }, FenceValue{ done[2], 1 } };
    device.queue(QueueType::Graphics).submit({}, joins, {});
    device.endFrame();
    for (const Fence fence : done)
    {
        REQUIRE(device.wait({ fence, 1 }, UINT64_MAX));
    }
    for (size_t type = 0; type < kTypes.size(); ++type)
    {
        for (size_t i = 0; i < targets[type].size(); ++i)
        {
            INFO("queue type " << type << ", list " << i);
            const std::vector<std::byte> expected(bytes.begin() + static_cast<ptrdiff_t>(i * kSlice), bytes.begin() + static_cast<ptrdiff_t>((i + 1) * kSlice));
            CHECK(test::readNativeBuffer(impl, resourceOf(device, targets[type][i]), kSlice) == expected);
        }
    }
    CHECK(messages.count() == 0u);
    for (size_t type = 0; type < kTypes.size(); ++type)
    {
        for (const Buffer target : targets[type])
        {
            device.destroy(target);
        }
        device.destroy(done[type]);
        device.destroy(sources[type]);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a submission with no lists executes nothing", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    uint64_t value = 0;
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        INFO("queue type " << static_cast<int>(type));
        const uint64_t before = device.counters().nativeSubmitCalls;
        const std::array<FenceValue, 1> signal{ FenceValue{ done, ++value } };
        device.queue(type).submit({}, {}, signal);
        // The signal alone, and on a compute or copy queue the progress signal of a submission that joins no frame.
        CHECK(device.counters().nativeSubmitCalls - before == (type == QueueType::Graphics ? 1u : 2u));
        CHECK(device.wait({ done, value }, UINT64_MAX));
    }
    CHECK(messages.count() == 0u);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the reuse of a frame slot resets the allocator of every list it handed out", "[d3d12]")
{
    for (const uint32_t framesInFlight : { 2u, 3u })
    {
        INFO("framesInFlight " << framesInFlight);
        Device device = makeDevice(framesInFlight);
        DeviceImpl& impl = DeviceImpl::of(device);
        const MessageCollection messages;
        const Fence computeDone = makeFence(device, "test.computeDone");
        const Fence copyDone = makeFence(device, "test.copyDone");
        // Frame 1 hands out four lists of its slot: two graphics, one compute and one copy.
        device.beginFrame();
        CommandList first = device.acquireCommandList(QueueType::Graphics, "test.first");
        first.end();
        device.queue(QueueType::Graphics).submit(std::span(&first, 1), {}, {});
        CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
        CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
        CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
        graphics.end();
        compute.end();
        copy.end();
        submitJoined(device, graphics, compute, copy, computeDone, copyDone, 1);
        device.endFrame();
        for (uint32_t frame = 2; frame <= framesInFlight; ++frame)
        {
            device.beginFrame();
            device.endFrame();
        }
        // Frame 1 is complete before the frame that reuses its slot begins, so that frame's wait reads the fence once.
        REQUIRE(d3d12::waitForFence(impl.frames.fence(), 1, kNativeWaitNs, "frame 1").reached);
        const uint64_t before = device.counters().nativeHostCalls;
        device.beginFrame();
        CHECK(device.counters().nativeHostCalls - before == 4u + 1u);
        // The reset allocators record again.
        CommandList again = device.acquireCommandList(QueueType::Graphics, "test.first");
        again.end();
        device.queue(QueueType::Graphics).submit(std::span(&again, 1), {}, {});
        device.endFrame();
        CHECK(messages.count() == 0u);
        device.destroy(computeDone);
        device.destroy(copyDone);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("a list carries its label and sets it again only when it changes", "[d3d12]")
{
    Device device = makeDevice(2);
    const MessageCollection messages;
    std::vector<ID3D12GraphicsCommandList9*> natives;
    for (const std::string_view label : { "test.first", "test.first", "test.first", "test.second" })
    {
        INFO("frame " << natives.size() + 1 << ", label " << label);
        device.beginFrame();
        const uint64_t before = device.counters().nativeHostCalls;
        CommandList list = device.acquireCommandList(QueueType::Graphics, label);
        const uint64_t named = device.counters().nativeHostCalls - before;
        natives.push_back(stateOf(list).list.get());
        // The first two frames make one list per slot; the third reuses the first's list under the same label, and the
        // fourth the second's under a new one.
        CHECK(named == (natives.size() == 3 ? 0u : 1u));
        CHECK(objectName(stateOf(list).list.get()) == wide(label));
        list.end();
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
        device.endFrame();
    }
    CHECK(natives[2] == natives[0]);
    CHECK(natives[3] == natives[1]);
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists acquired on four threads in one frame record into distinct native lists", "[d3d12]")
{
    constexpr std::array<std::string_view, 4> kLabels = { "test.thread0", "test.thread1", "test.thread2", "test.thread3" };
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    std::array<CommandList, 4> lists = {};
    std::array<std::thread, 4> threads;
    for (uint32_t i = 0; i < threads.size(); ++i)
    {
        // Each thread records while the others grow the same slot's pool.
        threads[i] = std::thread([&device, &lists, &kLabels, i] {
            CommandList list = device.acquireCommandList(QueueType::Graphics, kLabels[i]);
            for (uint32_t k = 0; k < 8; ++k)
            {
                (void)list.pushRoot(TestRoot{ { i, k, 0, 0 } });
            }
            list.end();
            lists[i] = list;
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    std::set<ID3D12GraphicsCommandList9*> natives;
    std::set<ID3D12CommandAllocator*> allocators;
    std::set<uint32_t> ordinals;
    for (uint32_t i = 0; i < lists.size(); ++i)
    {
        const d3d12::ListState& state = stateOf(lists[i]);
        natives.insert(state.list.get());
        allocators.insert(state.allocator.get());
        ordinals.insert(state.ordinal);
        CHECK(objectName(state.list.get()) == wide(kLabels[i]));
    }
    CHECK(natives.size() == 4u);
    CHECK(allocators.size() == 4u);
    CHECK(ordinals == std::set<uint32_t>{ 0, 1, 2, 3 });
    device.queue(QueueType::Graphics).submit(lists, {}, {});
    device.endFrame();
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("setRoot stores the buffer's GPU address plus the offset, marks both roots dirty and records nothing", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const Buffer buffer = makeBuffer(device, { .size = 4096, .label = "test.root" });
    const Fence computeDone = makeFence(device, "test.computeDone");
    device.beginFrame();
    std::array<CommandList, 2> lists = {};
    const std::array<QueueType, 2> types = { QueueType::Graphics, QueueType::Compute };
    for (size_t i = 0; i < lists.size(); ++i)
    {
        INFO("queue type " << static_cast<int>(types[i]));
        lists[i] = device.acquireCommandList(types[i], "test.list");
        const Addr root = device.address(buffer, 512);
        const uint64_t before = recordCalls(lists[i]);
        lists[i].setRoot(root);
        CHECK(recordCalls(lists[i]) - before == 0u);
        const d3d12::Binding& binding = stateOf(lists[i]).binding;
        CHECK(binding.root == resourceOf(device, buffer)->GetGPUVirtualAddress() + 512);
        const d3d12::BufferPayload* found = impl.directory.find(root.lo);
        REQUIRE(found != nullptr);
        CHECK(binding.root == found->base + root.hi);
        CHECK(binding.graphicsRootDirty);
        CHECK(binding.computeRootDirty);
        lists[i].end();
    }
    const std::array<FenceValue, 1> signal{ FenceValue{ computeDone, 1 } };
    device.queue(QueueType::Compute).submit(std::span(&lists[1], 1), {}, signal);
    device.queue(QueueType::Graphics).submit(std::span(&lists[0], 1), signal, {});
    device.endFrame();
    CHECK(device.wait({ computeDone, 1 }, UINT64_MAX));
    CHECK(messages.count() == 0u);
    device.destroy(computeDone);
    device.destroy(buffer);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("pushRoot binds the arena page's address plus its offset", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    (void)list.pushRoot(TestRoot{ { 1, 2, 3, 4 } });
    const uint64_t before = recordCalls(list);
    const Addr pushed = list.pushRoot(TestRoot{ { 5, 6, 7, 8 } });
    CHECK(recordCalls(list) - before == 0u);
    // The second root follows the first in the list's page, so its offset is not zero.
    CHECK(pushed.hi != 0u);
    const d3d12::BufferPayload* page = impl.directory.find(pushed.lo);
    REQUIRE(page != nullptr);
    const d3d12::Binding& binding = stateOf(list).binding;
    CHECK(binding.root == page->resource->GetGPUVirtualAddress() + pushed.hi);
    CHECK(binding.graphicsRootDirty);
    CHECK(binding.computeRootDirty);
    list.end();
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    device.endFrame();
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("empty lists of every queue type submit clean across six frames", "[d3d12]")
{
    for (const uint32_t framesInFlight : { 2u, 3u })
    {
        INFO("framesInFlight " << framesInFlight);
        Device device = makeDevice(framesInFlight);
        const MessageCollection messages;
        const Fence computeDone = makeFence(device, "test.computeDone");
        const Fence copyDone = makeFence(device, "test.copyDone");
        std::vector<ID3D12GraphicsCommandList9*> natives;
        for (uint64_t frame = 1; frame <= 6; ++frame)
        {
            device.beginFrame();
            CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
            CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
            CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
            natives.push_back(stateOf(graphics).list.get());
            graphics.end();
            compute.end();
            copy.end();
            submitJoined(device, graphics, compute, copy, computeDone, copyDone, frame);
            device.endFrame();
        }
        // A slot's reuse hands out the list, and the allocator, it made in its first frame.
        for (size_t frame = framesInFlight; frame < natives.size(); ++frame)
        {
            CHECK(natives[frame] == natives[frame - framesInFlight]);
        }
        CHECK(device.wait({ computeDone, 6 }, UINT64_MAX));
        CHECK(device.wait({ copyDone, 6 }, UINT64_MAX));
        CHECK(messages.count() == 0u);
        device.destroy(computeDone);
        device.destroy(copyDone);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("a texture's payload carries its description and copy format", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    std::vector<TextureDesc> descs = {
        { .format = Format::RGBA8Unorm_sRGB, .extent = { 16, 8, 1 }, .mips = 3, .usage = TextureUsage::Sampled, .label = "test.srgb" },
        { .format = Format::D32Float, .extent = { 16, 16, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.depth" },
        { .type = TextureType::CubeArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .mips = 2,
          .layers = 12,
          .usage = TextureUsage::Sampled,
          .label = "test.cubes" },
        { .type = TextureType::Tex3D, .format = Format::RGBA16Float, .extent = { 8, 8, 4 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.volume" },
    };
    if (device.formatCapabilities(Format::BC1Unorm).sampled)
    {
        descs.push_back({ .format = Format::BC1Unorm, .extent = { 16, 16, 1 }, .mips = 3, .usage = TextureUsage::Sampled, .label = "test.bc1" });
    }
    if ((device.formatCapabilities(Format::RGBA8Unorm).sampleCounts & 4u) != 0)
    {
        descs.push_back({ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .samples = 4, .usage = TextureUsage::RenderTarget, .label = "test.samples" });
    }
    for (const TextureDesc& desc : descs)
    {
        INFO("texture " << desc.label);
        Result<Texture> created = device.createTexture(desc);
        REQUIRE(created.has_value());
        const auto* payload = static_cast<const d3d12::TexturePayload*>(impl.core.pool<Texture>().native(*created));
        REQUIRE(payload != nullptr);
        CHECK(payload->format == desc.format);
        CHECK(payload->type == desc.type);
        CHECK(payload->usage == desc.usage);
        CHECK(payload->mips == desc.mips);
        CHECK(payload->layers == (desc.type == TextureType::Tex3D ? 1u : desc.layers));
        CHECK(payload->depth == desc.extent.depth);
        CHECK(payload->samples == desc.samples);
        d3d12::Com<ID3D12Resource2> described;
        REQUIRE(SUCCEEDED(payload->resource->QueryInterface(IID_PPV_ARGS(described.put()))));
        const D3D12_RESOURCE_DESC1 native = described->GetDesc1();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        impl.device()->GetCopyableFootprints1(&native, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        CHECK(payload->copyFormat != DXGI_FORMAT_UNKNOWN);
        CHECK(payload->copyFormat == footprint.Footprint.Format);
        device.destroy(*created);
    }
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}
