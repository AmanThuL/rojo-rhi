//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4FrameDataTests.cpp
/// @brief Tests Metal 4 frame data against the arena pages it lands in: their memory, residency, addresses and reuse.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The 64-bit value an address encodes.
    uint64_t valueOf(Addr addr)
    {
        return uint64_t{ addr.lo } | uint64_t{ addr.hi } << 32;
    }

    //==================================================================================================================
    /// The arena page `list` uploads into.
    Buffer pageOf(CommandList list)
    {
        return core::coreOf(list).page.buffer;
    }

    //==================================================================================================================
    /// The native buffer `buffer` wraps.
    MTL::Buffer* nativeOf(Device device, Buffer buffer)
    {
        return static_cast<MTL::Buffer*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
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
    /// Ends `list` and submits it to the graphics queue.
    void endAndSubmit(Device device, CommandList list)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("frame data lands in a resident Shared page at the address uploadFrameData returns", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::vector<std::byte> first = pattern(100, 1);
    const std::vector<std::byte> second = pattern(48, 200);
    const Addr firstAddr = list.uploadFrameData(first, 16);
    const Addr secondAddr = list.uploadFrameData(second, 256);
    const Buffer page = pageOf(list);
    MTL::Buffer* native = nativeOf(device, page);
    REQUIRE(native != nullptr);
    CHECK(native->storageMode() == MTL::StorageModeShared);
    CHECK(impl.residency.contains(native));
    {
        const metal4::AutoreleasePool pool;
        CHECK(metal4::toStdString(native->label()) == "rojorhi.frameArena.page");
    }
    for (const auto& [addr, bytes, alignment] : { std::tuple{ firstAddr, first, 16ull }, std::tuple{ secondAddr, second, 256ull } })
    {
        const uint64_t value = valueOf(addr);
        REQUIRE(value >= native->gpuAddress());
        const uint64_t offset = value - native->gpuAddress();
        CHECK(offset % alignment == 0);
        REQUIRE(offset + bytes.size() <= native->length());
        CHECK(std::memcmp(static_cast<const std::byte*>(native->contents()) + offset, bytes.data(), bytes.size()) == 0);
#ifndef NDEBUG
        // Only Debug builds decode addresses.
        const core::DecodedAddr decoded = core::decodeAddr(impl.core, addr);
        CHECK(decoded.slot == core::HandlePool<Buffer, core::BufferRecord>::slotIndex(page));
        CHECK(decoded.offset == offset);
#endif
    }
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a page is reused, not recreated, when its slot comes round again", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const std::vector<std::byte> bytes = pattern(64, 7);
    std::vector<MTL::Buffer*> pages;
    std::vector<uint64_t> growths;
    for (int frame = 0; frame < 4; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        const Addr addr = list.uploadFrameData(bytes, 16);
        MTL::Buffer* native = nativeOf(device, pageOf(list));
        CHECK(std::memcmp(static_cast<const std::byte*>(native->contents()) + (valueOf(addr) - native->gpuAddress()), bytes.data(), bytes.size()) == 0);
        pages.push_back(native);
        endAndSubmit(device, list);
        device.endFrame();
        growths.push_back(device.counters().frameArenaGrowths);
    }
    CHECK(pages[0] != pages[1]);
    CHECK(pages[2] == pages[0]);
    CHECK(pages[3] == pages[1]);
    CHECK(growths[1] == growths[0] + 1);
    CHECK(growths[3] == growths[1]);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upload larger than a standard page gets a page of its own size", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const std::vector<std::byte> bytes = pattern((2u << 20) + 5, 3);
    const Addr addr = list.uploadFrameData(bytes, 16);
    MTL::Buffer* native = nativeOf(device, pageOf(list));
    REQUIRE(native->length() >= bytes.size());
    CHECK(std::memcmp(static_cast<const std::byte*>(native->contents()) + (valueOf(addr) - native->gpuAddress()), bytes.data(), bytes.size()) == 0);
    endAndSubmit(device, list);
    device.endFrame();
    destroyDevice(device);
}
