//----------------------------------------------------------------------------------------------------------------------
/// @file DeviceDeathTests.cpp
/// @brief Death tests for the valid-usage rules of the device chapter and the object-handle and label rules it relies on.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-device-0001 framesInFlight is not 2 or 3", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0001", [] { (void)createDevice({ .framesInFlight = 4 }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0002 destroyDevice runs while a frame is open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0002", [] {
        Device device = *createDevice();
        device.beginFrame();
        destroyDevice(device);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0003 beginFrame runs while a frame is open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0003", [] {
        Device device = *createDevice();
        device.beginFrame();
        device.beginFrame();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0004 endFrame runs with no frame open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0004", [] {
        Device device = *createDevice();
        device.endFrame();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed Device calls a member", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        Device device;
        device.beginFrame();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0006 a label is not valid UTF-8", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0006", [] { (void)createDevice({ .label = "bad\xFF" }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0007 a label contains a NUL character", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0007", [] { (void)createDevice({ .label = std::string_view("a\0b", 3) }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed Swapchain is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        Device device = *createDevice();
        device.destroy(Swapchain{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0001 a null Buffer is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0001", [] {
        Device device = *createDevice();
        device.destroy(Buffer{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0002 a Buffer is destroyed twice", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0002", [] {
        Device device = *createDevice();
        const Buffer buffer = core::coreOf(device).pool<Buffer>().allocate({ "test.buffer" });
        device.destroy(buffer);
        device.destroy(buffer);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0003 a Buffer's bits are destroyed as a Texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0003", [] {
        Device device = *createDevice();
        const Buffer buffer = core::coreOf(device).pool<Buffer>().allocate({ "test.buffer" });
        device.destroy(Texture{ buffer.bits });
    });
}
