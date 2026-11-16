//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12BufferTests.cpp
/// @brief Tests D3D12 buffers: their memory, initial data, raw views, addresses, retirement and frame data.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/SlotAllocator.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
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

    /// Collects the library's warnings and errors, the debug layer's among them, into `g_messages`, emptied first, while it
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

        /// Returns the messages collected so far, listing them for the log.
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
    /// Returns the payload of `buffer`, a live buffer of `device`.
    const d3d12::BufferPayload& payloadOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return *payload;
    }

    //==================================================================================================================
    /// Creates a buffer on `device`, failing the case when it cannot.
    Buffer makeBuffer(Device device, const BufferDesc& desc, std::span<const std::byte> initial = {}, Placement placement = {})
    {
        Result<Buffer> created = device.createBuffer(desc, initial, placement);
        INFO("createBuffer: " << (created ? std::string("ok") : created.error().message));
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns the slot index of `buffer` in the buffer pool.
    uint32_t slotOf(Buffer buffer)
    {
        return core::HandlePool<Buffer, core::BufferRecord>::slotIndex(buffer);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Gpu buffer's initial data is in its memory when createBuffer returns", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    const std::vector<std::byte> bytes = pattern(1'000, 7);
    const Buffer buffer = makeBuffer(*created, { .size = 1'000 }, bytes);
    CHECK(test::readNativeBuffer(impl, payloadOf(*created, buffer).resource, 1'000) == bytes);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a placed Gpu buffer's initial data is in its memory when createBuffer returns", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    const BufferDesc desc{ .size = 1'000 };
    Result<Heap> heap = created->createHeap({ .size = created->placementRequirements(desc).size, .label = "test.heap" });
    REQUIRE(heap.has_value());
    const std::vector<std::byte> bytes = pattern(1'000, 11);
    const Buffer buffer = makeBuffer(*created, desc, bytes, { *heap, 0 });
    CHECK(test::readNativeBuffer(impl, payloadOf(*created, buffer).resource, 1'000) == bytes);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("memory classes map to the D3D12 heap types", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const bool gpuUpload = DeviceImpl::of(*created).features.gpuUploadHeap;
    const std::array<std::pair<Memory, D3D12_HEAP_TYPE>, 3> expected = {
        std::pair{ Memory::Gpu, D3D12_HEAP_TYPE_DEFAULT },
        std::pair{ Memory::Upload, gpuUpload ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD },
        std::pair{ Memory::Readback, D3D12_HEAP_TYPE_READBACK },
    };
    for (const auto& [memory, type] : expected)
    {
        const Buffer buffer = makeBuffer(*created, { .size = 256, .memory = memory });
        D3D12_HEAP_PROPERTIES properties = {};
        D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
        REQUIRE(SUCCEEDED(payloadOf(*created, buffer).resource->GetHeapProperties(&properties, &flags)));
        INFO("memory " << static_cast<int>(memory));
        CHECK(properties.Type == type);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("Device::address is the raw SRV index and the offset", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    core::SlotAllocator& views = DeviceImpl::of(*created).core.views();
    for (const BufferUsage usage : { BufferUsage::ShaderWrite, BufferUsage::None })
    {
        const Buffer buffer = makeBuffer(*created, { .size = 256, .usage = usage });
        const d3d12::BufferPayload& payload = payloadOf(*created, buffer);
        const Addr address = created->address(buffer, 12);
        CHECK(address.lo == payload.viewIndex);
        CHECK(address.hi == 12u);
        CHECK(payload.viewIndex != 0u);
        CHECK(views.holdsRange(payload.viewIndex, usage == BufferUsage::ShaderWrite ? 2u : 1u));
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("an address decodes to its buffer", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const Buffer buffer = makeBuffer(*created, { .size = 256 });
    const core::DecodedAddr decoded = core::decodeAddr(DeviceImpl::of(*created).core, created->address(buffer, 40));
    CHECK(decoded.slot == slotOf(buffer));
    CHECK(decoded.offset == 40u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a Readback buffer takes no heap slot", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const uint64_t before = created->counters().descriptorWrites;
    const Buffer buffer = makeBuffer(*created, { .size = 256, .memory = Memory::Readback });
    CHECK(payloadOf(*created, buffer).viewCount == 0u);
    CHECK(created->counters().descriptorWrites == before);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a 3-byte buffer and a (2^29 + 4)-byte buffer get raw views with no debug-layer message", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    {
        const MessageCollection messages;
        constexpr uint64_t kBig = (1ull << 29) + 4;
        const Buffer tiny = makeBuffer(*created, { .size = 3, .usage = BufferUsage::ShaderWrite });
        const Buffer big = makeBuffer(*created, { .size = kBig, .usage = BufferUsage::ShaderWrite });
        CHECK(payloadOf(*created, tiny).resource->GetDesc().Width == 4u);
        CHECK(payloadOf(*created, big).resource->GetDesc().Width == kBig);
        CHECK(payloadOf(*created, tiny).viewCount == 2u);
        CHECK(payloadOf(*created, big).viewCount == 2u);
        const Addr last = created->address(big, d3d12::kMaxAddrOffset - 1);
        CHECK(last.lo == payloadOf(*created, big).viewIndex);
        CHECK(last.hi == d3d12::kMaxAddrOffset - 1);
        created->destroy(tiny);
        created->destroy(big);
        CHECK(messages.count() == 0u);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a destroyed buffer's raw view pair is not reused before its frame retires", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    const BufferDesc desc{ .size = 256, .usage = BufferUsage::ShaderWrite };
    created->beginFrame();
    const Buffer destroyed = makeBuffer(*created, desc);
    const d3d12::BufferPayload* payload = &payloadOf(*created, destroyed);
    const uint32_t index = payload->viewIndex;
    created->destroy(destroyed);
    const std::set<uint32_t> taken = { index, index + 1 };
    std::vector<Buffer> later = { makeBuffer(*created, desc) };
    CHECK_FALSE(taken.contains(payloadOf(*created, later.back()).viewIndex));
    CHECK_FALSE(taken.contains(payloadOf(*created, later.back()).viewIndex + 1));
    CHECK(impl.directory.find(index) == payload);
    for (uint32_t frame = 1; frame < impl.core.framesInFlight(); ++frame)
    {
        created->endFrame();
        created->beginFrame();
        later.push_back(makeBuffer(*created, desc));
        CHECK_FALSE(taken.contains(payloadOf(*created, later.back()).viewIndex));
        CHECK_FALSE(taken.contains(payloadOf(*created, later.back()).viewIndex + 1));
        CHECK(impl.directory.find(index) == payload);
    }
    created->endFrame();
    // This frame reuses the slot of the frame the buffer was destroyed in, so the buffer retires.
    created->beginFrame();
    CHECK(impl.directory.find(index) == nullptr);
    created->endFrame();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("buffers created on four threads decode to their own slots", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    constexpr uint32_t kThreads = 4;
    constexpr uint32_t kBuffers = 500;
    const std::vector<std::byte> bytes = pattern(16, 3);
    std::array<std::vector<Buffer>, kThreads> buffers;
    std::atomic<uint32_t> failures = 0;
    {
        std::array<std::jthread, kThreads> threads;
        for (uint32_t t = 0; t < kThreads; ++t)
        {
            threads[t] = std::jthread([device = *created, &bytes, &buffers, &failures, t]() mutable {
                for (uint32_t i = 0; i < kBuffers; ++i)
                {
                    Result<Buffer> buffer = device.createBuffer({ .size = 16 }, bytes);
                    if (!buffer)
                    {
                        failures.fetch_add(1);
                        continue;
                    }
                    buffers[t].push_back(*buffer);
                }
            });
        }
    }
    REQUIRE(failures.load() == 0u);
    core::DeviceCore& core = DeviceImpl::of(*created).core;
    std::set<uint32_t> indices;
    uint32_t wrong = 0;
    for (const std::vector<Buffer>& list : buffers)
    {
        for (const Buffer buffer : list)
        {
            CHECK(indices.insert(payloadOf(*created, buffer).viewIndex).second);
            if (core::decodeAddr(core, created->address(buffer)).slot != slotOf(buffer))
            {
                ++wrong;
            }
        }
    }
    CHECK(indices.size() == kThreads * kBuffers);
    CHECK(wrong == 0u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("frame data addresses name the arena page's raw view", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    created->beginFrame();
    CommandList list = created->acquireCommandList(QueueType::Graphics, "test.list");
    const std::vector<std::byte> bytes = pattern(16, 5);
    const Addr address = list.uploadFrameData(bytes);
    const d3d12::BufferPayload* page = impl.directory.find(address.lo);
    REQUIRE(page != nullptr);
    REQUIRE(page->host != nullptr);
    CHECK(std::memcmp(page->host + address.hi, bytes.data(), bytes.size()) == 0);
    list.end();
    const std::array<CommandList, 1> lists = { list };
    created->queue(QueueType::Graphics).submit(lists);
    created->endFrame();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a buffer creation counts its raw views as descriptor writes", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const uint64_t before = created->counters().descriptorWrites;
    (void)makeBuffer(*created, { .size = 64, .usage = BufferUsage::ShaderWrite });
    CHECK(created->counters().descriptorWrites == before + 2);
    (void)makeBuffer(*created, { .size = 64, .memory = Memory::Upload });
    CHECK(created->counters().descriptorWrites == before + 3);
    destroyDevice(*created);
}
