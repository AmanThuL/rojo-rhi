//----------------------------------------------------------------------------------------------------------------------
/// @file NullBufferTests.cpp
/// @brief Tests what only the Null backend promises about buffers: the address encoding and host memory.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    uint32_t slotOf(Buffer buffer)
    {
        return core::HandlePool<Buffer, core::BufferRecord>::slotIndex(buffer);
    }

    //==================================================================================================================
    void* nativeOf(Device device, Buffer buffer)
    {
        return core::coreOf(device).pool<Buffer>().lookup(buffer).record->native;
    }

    /// Wraps the Null buffer hook: forwards each retiring record, then notes the frame and whether the payload was cleared.
    struct Spy
    {
        core::RetireHooks inner;
        core::DeviceCore* core = nullptr;
        std::vector<std::pair<uint64_t, bool>> seen;
    };

    //==================================================================================================================
    void spyOnBuffer(void* context, core::BufferRecord& record)
    {
        Spy& spy = *static_cast<Spy*>(context);
        spy.inner.buffer(spy.inner.context, record);
        spy.seen.emplace_back(spy.core->frameNumber(), record.native == nullptr);
    }

    //==================================================================================================================
    void spyOnSlot(void* context, uint32_t slot)
    {
        Spy& spy = *static_cast<Spy*>(context);
        spy.inner.frameSlot(spy.inner.context, slot);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Null address holds the buffer's slot index in lo and the offset in hi", "[null]")
{
    Device device = *createDevice();
    const Buffer first = *device.createBuffer({ .size = 1024 });
    const Buffer second = *device.createBuffer({ .size = 1024 });
    CHECK(device.address(first).lo == slotOf(first));
    CHECK(device.address(second).lo == slotOf(second));
    CHECK(device.address(first).lo != 0);
    CHECK(device.address(first).hi == 0);
    CHECK(device.address(first, 300).hi == 300);
    CHECK(device.address(first, 300).lo == device.address(first).lo);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Null operator+ adds to the offset word and keeps the index word", "[null]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 1024 });
    const Addr moved = device.address(buffer, 100) + 28;
    CHECK(moved.lo == slotOf(buffer));
    CHECK(moved.hi == 128);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an address one byte below maxAddrOffset is accepted", "[null]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 1ull << 32 }); // A Gpu buffer holds no host memory.
    const Addr reached = device.address(buffer) + (device.capabilities().limits.maxAddrOffset - 1);
    CHECK(reached.hi == 0xFFFF'FFFEu);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an Upload buffer without initial data is zero-filled", "[null]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 32, .memory = Memory::Upload });
    const std::byte* pointer = device.cpuPointer(buffer);
    bool zero = true;
    for (int i = 0; i < 32; ++i)
    {
        zero = zero && pointer[i] == std::byte{ 0 };
    }
    CHECK(zero);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the unwritten tail of an Upload buffer with initial data is zero", "[null]")
{
    Device device = *createDevice();
    const std::array<std::byte, 2> bytes = { std::byte{ 5 }, std::byte{ 6 } };
    const Buffer buffer = *device.createBuffer({ .size = 8, .memory = Memory::Upload }, bytes);
    const std::byte* pointer = device.cpuPointer(buffer);
    CHECK(pointer[1] == std::byte{ 6 });
    CHECK(pointer[2] == std::byte{ 0 });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("only Upload and Readback buffers hold host memory", "[null]")
{
    Device device = *createDevice();
    CHECK(nativeOf(device, *device.createBuffer({ .size = 100, .memory = Memory::Gpu })) == nullptr);
    CHECK(nativeOf(device, *device.createBuffer({ .size = 64, .memory = Memory::Upload })) != nullptr);
    CHECK(nativeOf(device, *device.createBuffer({ .size = 16, .memory = Memory::Readback })) != nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("host memory is freed when the frame that released the buffer retires", "[null]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    core::DeviceCore& core = core::coreOf(device);
    Spy spy;
    spy.inner = core.retireHooks();
    spy.core = &core;
    core::RetireHooks hooks = spy.inner;
    hooks.context = &spy;
    hooks.buffer = spyOnBuffer;
    hooks.frameSlot = spyOnSlot;
    core.setRetireHooks(hooks);

    device.beginFrame();
    const Buffer buffer = *device.createBuffer({ .size = 64, .memory = Memory::Upload });
    device.destroy(buffer);
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    CHECK(spy.seen.empty());
    device.beginFrame(); // Frame 3 retires frame 1.
    REQUIRE(spy.seen.size() == 1);
    CHECK(spy.seen[0].first == 3);
    CHECK(spy.seen[0].second);
    device.endFrame();
    destroyDevice(device);
}
