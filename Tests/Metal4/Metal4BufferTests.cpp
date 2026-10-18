//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4BufferTests.cpp
/// @brief Tests Metal 4 buffers and heaps against the native objects they wrap: memory, addresses, residency and staging.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Memory.h"
#include "Support/Metal4/Metal4Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The native buffer `buffer` wraps.
    MTL::Buffer* nativeOf(Device device, Buffer buffer)
    {
        return static_cast<MTL::Buffer*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
    }

    //==================================================================================================================
    /// The native heap `heap` wraps.
    MTL::Heap* nativeOf(Device device, Heap heap)
    {
        return static_cast<MTL::Heap*>(DeviceImpl::of(device).core.pool<Heap>().native(heap));
    }

    //==================================================================================================================
    /// The slot index `buffer` names in the buffer pool.
    uint32_t slotOf(Buffer buffer)
    {
        return core::HandlePool<Buffer, core::BufferRecord>::slotIndex(buffer);
    }

    //==================================================================================================================
    /// The 64-bit value an address encodes.
    uint64_t valueOf(Addr addr)
    {
        return uint64_t{ addr.lo } | uint64_t{ addr.hi } << 32;
    }

    //==================================================================================================================
    /// `count` bytes counting up from `first`, wrapping at 256.
    std::vector<std::byte> pattern(size_t count, uint8_t first)
    {
        std::vector<std::byte> bytes(count);
        for (size_t i = 0; i < count; ++i)
        {
            bytes[i] = static_cast<std::byte>(static_cast<uint8_t>(first + i));
        }
        return bytes;
    }

    //==================================================================================================================
    /// Runs `count` empty frames on `device`.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Gpu buffer's initial data is in its memory when createBuffer returns", "[metal4]")
{
    Device device = *createDevice();
    // Shorter than the buffer and not a multiple of four: only these bytes are copied.
    const std::vector<std::byte> bytes = pattern(37, 11);
    const Buffer buffer = *device.createBuffer({ .size = 64 }, bytes);
    const std::vector<std::byte> read = test::readBuffer(device, buffer);
    REQUIRE(read.size() == 64);
    CHECK(std::memcmp(read.data(), bytes.data(), bytes.size()) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a placed Gpu buffer's initial data is in its memory when createBuffer returns", "[metal4]")
{
    Device device = *createDevice();
    const BufferDesc desc{ .size = 4096 };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size * 2 });
    const std::vector<std::byte> bytes = pattern(4096, 200);
    const Buffer buffer = *device.createBuffer(desc, bytes, { heap, requirements.size });
    const std::vector<std::byte> read = test::readBuffer(device, buffer);
    REQUIRE(read.size() == bytes.size());
    CHECK(std::memcmp(read.data(), bytes.data(), bytes.size()) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Gpu memory is Private and Upload memory is Shared with the default CPU cache mode", "[metal4]")
{
    Device device = *createDevice();
    const metal4::AutoreleasePool pool;
    MTL::Buffer* gpu = nativeOf(device, *device.createBuffer({ .size = 64 }));
    MTL::Buffer* upload = nativeOf(device, *device.createBuffer({ .size = 64, .memory = Memory::Upload }));
    MTL::Buffer* readback = nativeOf(device, *device.createBuffer({ .size = 64, .memory = Memory::Readback }));
    CHECK(gpu->storageMode() == MTL::StorageModePrivate);
    CHECK(upload->storageMode() == MTL::StorageModeShared);
    CHECK(upload->cpuCacheMode() == MTL::CPUCacheModeDefaultCache);
    CHECK(readback->storageMode() == MTL::StorageModeShared);
    for (const Memory memory : { Memory::Gpu, Memory::Upload, Memory::Readback })
    {
        CAPTURE(static_cast<int>(memory));
        const Heap heap = *device.createHeap({ .size = 1 << 16, .memory = memory });
        MTL::Heap* native = nativeOf(device, heap);
        CHECK(native->type() == MTL::HeapTypePlacement);
        CHECK(native->hazardTrackingMode() == MTL::HazardTrackingModeUntracked);
        CHECK(native->storageMode() == (memory == Memory::Gpu ? MTL::StorageModePrivate : MTL::StorageModeShared));
        CHECK(native->cpuCacheMode() == MTL::CPUCacheModeDefaultCache);
        MTL::Buffer* placed = nativeOf(device, *device.createBuffer({ .size = 64, .memory = memory }, {}, { heap, 0 }));
        CHECK(placed->storageMode() == native->storageMode());
        CHECK(placed->hazardTrackingMode() == MTL::HazardTrackingModeUntracked);
        CHECK(placed->heap() == native);
    }
    CHECK(DeviceImpl::of(device).core.pool<Heap>().native(Heap{}) == nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Device::address is the buffer's GPU address plus the offset, split into lo and hi", "[metal4]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 256, .memory = Memory::Upload });
    const uint64_t base = nativeOf(device, buffer)->gpuAddress();
    REQUIRE(base != 0);
    for (const uint64_t offset : { 0ull, 1ull, 255ull })
    {
        CAPTURE(offset);
        const Addr addr = device.address(buffer, offset);
        CHECK(addr.lo == static_cast<uint32_t>(base + offset));
        CHECK(addr.hi == static_cast<uint32_t>((base + offset) >> 32));
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an address decodes to its buffer and offset, and an address outside every buffer to slot 0", "[metal4]")
{
#ifdef NDEBUG
    SKIP("only Debug builds decode addresses, so only they keep the address map");
#endif
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    const Buffer first = *device.createBuffer({ .size = 256 });
    const Buffer second = *device.createBuffer({ .size = 4096, .memory = Memory::Upload });
    for (const Buffer buffer : { first, second })
    {
        const core::DecodedAddr start = core::decodeAddr(core, device.address(buffer));
        CHECK(start.slot == slotOf(buffer));
        CHECK(start.offset == 0);
        const core::DecodedAddr inside = core::decodeAddr(core, device.address(buffer, 200));
        CHECK(inside.slot == slotOf(buffer));
        CHECK(inside.offset == 200);
    }
    const core::DecodedAddr outside = core::decodeAddr(core, Addr{ 16, 0 });
    CHECK(outside.slot == 0);
    CHECK(outside.offset == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed buffer's address decodes to its slot until the frame it was destroyed in retires", "[metal4]")
{
#ifdef NDEBUG
    SKIP("only Debug builds decode addresses, so only they keep the address map");
#endif
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = core::coreOf(device);
    const Buffer buffer = *device.createBuffer({ .size = 256 });
    const Addr addr = device.address(buffer, 8);
    device.destroy(buffer);
    CHECK(core::decodeAddr(core, addr).slot == slotOf(buffer));
    runFrames(device, 2);
    CHECK(core::decodeAddr(core, addr).slot == slotOf(buffer));
    runFrames(device, 1);
    CHECK(core::decodeAddr(core, addr).slot == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an address inside aliased placed buffers decodes to one that is not destroyed", "[metal4]")
{
#ifdef NDEBUG
    SKIP("only Debug builds decode addresses, so only they keep the address map");
#endif
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    const BufferDesc large{ .size = 1 << 20 };
    const BufferDesc small{ .size = 256 };
    const uint64_t innerOffset = device.placementRequirements(small).alignment;
    const Heap heap = *device.createHeap({ .size = device.placementRequirements(large).size });
    const Buffer whole = *device.createBuffer(large, {}, { heap, 0 });
    const Buffer front = *device.createBuffer(small, {}, { heap, 0 });
    const Buffer inner = *device.createBuffer(small, {}, { heap, innerOffset });
    // An address past both small buffers is only inside the large one.
    const core::DecodedAddr past = core::decodeAddr(core, device.address(whole, 1 << 19));
    CHECK(past.slot == slotOf(whole));
    CHECK(past.offset == 1 << 19);
    // Where a destroyed buffer aliases the live one, the destroyed buffer is passed over.
    device.destroy(front);
    device.destroy(inner);
    const core::DecodedAddr sameBase = core::decodeAddr(core, device.address(whole, 16));
    CHECK(sameBase.slot == slotOf(whole));
    CHECK(sameBase.offset == 16);
    const core::DecodedAddr insideInner = core::decodeAddr(core, device.address(whole, innerOffset + 16));
    CHECK(insideInner.slot == slotOf(whole));
    CHECK(insideInner.offset == innerOffset + 16);
    device.destroy(whole);
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dedicated buffer is resident until its frame retires; a placed one never joins, its heap does", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    const Buffer dedicated = *device.createBuffer({ .size = 256 });
    MTL::Buffer* native = nativeOf(device, dedicated);
    // Kept alive past retirement, so the set can still be asked about it.
    native->retain();
    CHECK(impl.residency.contains(native));
    const Heap heap = *device.createHeap({ .size = 1 << 16 });
    const Buffer placed = *device.createBuffer({ .size = 256 }, {}, { heap, 0 });
    CHECK(impl.residency.contains(nativeOf(device, heap)));
    CHECK_FALSE(impl.residency.contains(nativeOf(device, placed)));
    device.destroy(dedicated);
    runFrames(device, 2);
    CHECK(impl.residency.contains(native));
    runFrames(device, 1);
    CHECK_FALSE(impl.residency.contains(native));
    {
        const metal4::AutoreleasePool pool;
        native->release();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("placement requirements are the device's size and alignment for the placed options", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = DeviceImpl::of(device).device;
    for (const Memory memory : { Memory::Gpu, Memory::Upload, Memory::Readback })
    {
        for (const uint64_t size : { 1ull, 1000ull, 65536ull + 3 })
        {
            CAPTURE(static_cast<int>(memory), size);
            const PlacementRequirements requirements = device.placementRequirements(BufferDesc{ .size = size, .memory = memory });
            const MTL::SizeAndAlign expected = native->heapBufferSizeAndAlign(size, metal4::placedOptions(memory));
            CHECK(requirements.size == expected.size);
            CHECK(requirements.alignment == expected.align);
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a staged creation counts one staging buffer and one submission", "[metal4]")
{
    Device device = *createDevice();
    const std::array<std::byte, 16> bytes{};
    const Counters start = device.counters();
    (void)*device.createBuffer({ .size = 64 });
    const Counters plain = device.counters();
    (void)*device.createBuffer({ .size = 64 }, bytes);
    const Counters staged = device.counters();
    CHECK(plain.nativeSubmitCalls == start.nativeSubmitCalls);
    CHECK(staged.nativeSubmitCalls == plain.nativeSubmitCalls + 1);
    CHECK(staged.nativeCreateCalls - plain.nativeCreateCalls == plain.nativeCreateCalls - start.nativeCreateCalls + 1);
    CHECK(staged.gpuAllocations - plain.gpuAllocations == plain.gpuAllocations - start.gpuAllocations);
    // The new buffer joined the device set, which the staging commit makes resident first; the staging buffer joins and
    // leaves the stager's own set, one commit each.
    CHECK(staged.nativeHostCalls == plain.nativeHostCalls + 3);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("operator+ adds 64-bit offsets across the lo word", "[metal4]")
{
    Device device = *createDevice();
    const Addr base = device.address(*device.createBuffer({ .size = 64 }));
    for (const uint64_t offset : { 0ull, 1ull, 0xFFFF'FFFFull, (1ull << 32) + 3 })
    {
        CAPTURE(offset);
        const Addr sum = base + offset;
        CHECK(valueOf(sum) == valueOf(base) + offset);
    }
    const Addr carry = Addr{ 0xFFFF'FFF0u, 7 } + 0x20;
    CHECK(carry.lo == 0x10);
    CHECK(carry.hi == 8);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("devices that stage initial data are created and destroyed a hundred times", "[metal4]")
{
    // A device destroyed straight after a staged creation must not leave its last release to Metal's notification thread.
    const std::array<std::byte, 64> bytes{};
    for (int round = 0; round < 100; ++round)
    {
        Device device = *createDevice();
        CHECK(device.createBuffer({ .size = 256 }, bytes).has_value());
        destroyDevice(device);
    }
}
