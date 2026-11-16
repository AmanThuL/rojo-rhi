//----------------------------------------------------------------------------------------------------------------------
/// @file NullCommandListTests.cpp
/// @brief Tests what only the Null backend promises about frame data: its address encoding, its arena pages and their reuse.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr std::array<std::byte, 16> kBytes = {};

    //==================================================================================================================
    /// Opens a frame and returns a Graphics list of it.
    CommandList beginFrameWithList(Device device)
    {
        device.beginFrame();
        return device.acquireCommandList(QueueType::Graphics, "null.list");
    }

    //==================================================================================================================
    void endFrameWithList(Device device, CommandList list)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
        device.endFrame();
    }

    //==================================================================================================================
    size_t arenaPageCount(Device device)
    {
        size_t count = 0;
        core::coreOf(device).pool<Buffer>().forEachLive([&](const core::BufferRecord& record) { count += record.label == "rojorhi.frameArena.page" ? 1 : 0; });
        return count;
    }

    //==================================================================================================================
    /// Returns the host memory of the arena page whose slot index is `lo`, the word the Null encoding puts the slot in.
    std::byte* pageMemory(Device device, uint32_t lo)
    {
        for (const Buffer page : core::coreOf(device).arena().pageBuffers())
        {
            if (core::HandlePool<Buffer, core::BufferRecord>::slotIndex(page) == lo)
            {
                return device.cpuPointer(page);
            }
        }
        return nullptr;
    }

    [[maybe_unused]] std::vector<std::string> gWarnings;

    //==================================================================================================================
    [[maybe_unused]] void collectWarning(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Warning)
        {
            gWarnings.emplace_back(message);
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("the first upload of a frame lands at offset zero of an arena page", "[null]")
{
    Device device = *createDevice();
    CommandList list = beginFrameWithList(device);
    CHECK(list.uploadFrameData(kBytes).hi == 0);
    endFrameWithList(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("uploads of one page share the slot word and advance by their alignment", "[null]")
{
    Device device = *createDevice();
    CommandList list = beginFrameWithList(device);
    const std::array<std::byte, 10> ten = {};
    const Addr first = list.uploadFrameData(ten, 256);
    const Addr second = list.uploadFrameData(ten, 256);
    const Addr third = list.uploadFrameData(std::span<const std::byte>(ten).first(1), 16);
    CHECK(first.hi == 0);
    CHECK(second.hi == 256);
    CHECK(third.hi == 272);
    CHECK(second.lo == first.lo);
    CHECK(third.lo == first.lo);
    endFrameWithList(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("uploaded bytes are copied to the page", "[null]")
{
    Device device = *createDevice();
    CommandList list = beginFrameWithList(device);
    std::array<std::byte, 4> payload = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
    (void)list.uploadFrameData(kBytes);
    const Addr address = list.uploadFrameData(payload);
    const std::byte* memory = pageMemory(device, address.lo);
    REQUIRE(memory != nullptr);
    CHECK(std::memcmp(memory + address.hi, payload.data(), payload.size()) == 0);
    endFrameWithList(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an arena page is a live buffer of the device", "[null]")
{
    Device device = *createDevice();
    CHECK(arenaPageCount(device) == 0);
    CommandList list = beginFrameWithList(device);
    (void)list.uploadFrameData(kBytes);
    CHECK(arenaPageCount(device) == 1);
    endFrameWithList(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a slot's arena rewinds when the slot is reused", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    CommandList first = beginFrameWithList(device);
    const Addr inFrameOne = first.uploadFrameData(kBytes);
    endFrameWithList(device, first);

    CommandList second = beginFrameWithList(device);
    const Addr inFrameTwo = second.uploadFrameData(kBytes);
    endFrameWithList(device, second);

    CommandList third = beginFrameWithList(device);
    const Addr inFrameThree = third.uploadFrameData(kBytes);
    endFrameWithList(device, third);

    CHECK(inFrameTwo.lo != inFrameOne.lo);
    CHECK(inFrameThree.lo == inFrameOne.lo);
    CHECK(inFrameThree.hi == inFrameOne.hi);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a full page is followed by a new page and a rewound slot keeps both", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    const std::vector<std::byte> large(600 * 1024);
    for (int frame = 0; frame < 6; ++frame)
    {
        CommandList list = beginFrameWithList(device);
        const Addr a = list.uploadFrameData(large);
        const Addr b = list.uploadFrameData(large);
        CHECK(a.lo != b.lo);
        endFrameWithList(device, list);
    }
    // Two slots of two pages each; no frame after the first two creates another.
    CHECK(arenaPageCount(device) == 4);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upload above the page size gets a page of its own", "[null]")
{
    Device device = *createDevice();
    CommandList list = beginFrameWithList(device);
    const std::vector<std::byte> big(3u << 20);
    const Addr large = list.uploadFrameData(big);
    const Addr small = list.uploadFrameData(kBytes);
    CHECK(large.hi == 0);
    CHECK(small.lo != large.lo);
    endFrameWithList(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice with validation does not warn of arena pages", "[null]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    gWarnings.clear();
    setMessageCallback(collectWarning, nullptr);
    Device device = *createDevice(DeviceDesc{ .validation = true });
    CommandList list = beginFrameWithList(device);
    (void)list.uploadFrameData(kBytes);
    endFrameWithList(device, list);
    destroyDevice(device);
    setMessageCallback(nullptr, nullptr);
    CHECK(gWarnings.empty());
#endif
}
