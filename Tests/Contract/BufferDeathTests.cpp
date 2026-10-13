//----------------------------------------------------------------------------------------------------------------------
/// @file BufferDeathTests.cpp
/// @brief Death tests for the valid-usage rules of the buffer chapter that the buffer entries check.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0003 ShaderWrite is set on an Upload buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0003", [] {
        Device device = *createDevice();
        (void)device.createBuffer({ .size = 64, .memory = Memory::Upload, .usage = BufferUsage::ShaderWrite });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0004 Index is set on a Readback buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0004", [] {
        Device device = *createDevice();
        (void)device.createBuffer({ .size = 64, .memory = Memory::Readback, .usage = BufferUsage::Index });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0005 Sparse is set on a device without sparse resources", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0005", [] {
        Device device = *createDevice();
        core::coreOf(device).capabilities().placementSparse = false;
        (void)device.createBuffer({ .size = 64, .usage = BufferUsage::Sparse });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0006 a buffer has size zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0006", [] {
        Device device = *createDevice();
        (void)device.createBuffer({ .size = 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0007 initial data is larger than the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0007", [] {
        Device device = *createDevice();
        const std::byte bytes[16] = {};
        (void)device.createBuffer({ .size = 8 }, bytes);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0008 initial data is given for a Readback buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0008", [] {
        Device device = *createDevice();
        const std::byte bytes[4] = {};
        (void)device.createBuffer({ .size = 8, .memory = Memory::Readback }, bytes);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0011 Addr arithmetic reaches maxAddrOffset", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0011", [] {
        Device device = *createDevice();
        const Buffer buffer = *device.createBuffer({ .size = 1ull << 32 });
        (void)(device.address(buffer) + 0xFFFF'FFFFull);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0012 the address offset is the buffer size", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0012", [] {
        Device device = *createDevice();
        const Buffer buffer = *device.createBuffer({ .size = 64 });
        (void)device.address(buffer, 64);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0013 the address of a Readback buffer is taken", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0013", [] {
        Device device = *createDevice();
        const Buffer buffer = *device.createBuffer({ .size = 64, .memory = Memory::Readback });
        (void)device.address(buffer);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0014 the CPU pointer of a Gpu buffer is taken", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0014", [] {
        Device device = *createDevice();
        const Buffer buffer = *device.createBuffer({ .size = 64 });
        (void)device.cpuPointer(buffer);
    });
}
