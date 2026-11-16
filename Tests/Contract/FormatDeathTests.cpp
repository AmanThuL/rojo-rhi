//----------------------------------------------------------------------------------------------------------------------
/// @file FormatDeathTests.cpp
/// @brief Death tests for the valid-usage rules of the formats chapter: the format queries and their use by a texture or a view.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-format-0001 a Format value is not an enumerator", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0001", [] {
        Device device = *createDevice();
        (void)device.formatCapabilities(static_cast<Format>(14));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0005 formatCapabilities is asked about Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0005", [] {
        Device device = *createDevice();
        (void)device.formatCapabilities(Format::Undefined);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a Sampled texture is created in a format the device cannot sample", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        FormatCapabilities capabilities = device.formatCapabilities(Format::RGBA8Unorm);
        capabilities.sampled = false;
        core::coreOf(device).setFormatCapabilities(Format::RGBA8Unorm, capabilities);
        (void)device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a Storage view reinterprets the format as one without storage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        Device device = *createDevice();
        const Texture texture = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Storage });
        (void)device.createView(texture, TextureViewDesc{ .format = Format::RGBA8Unorm_sRGB, .usage = TextureUsage::Storage });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0004 a block-compressed texture is not a multiple of 4 wide", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0004", [] {
        Device device = *createDevice();
        (void)device.createTexture({ .format = Format::BC1Unorm, .extent = { 6, 8, 1 }, .usage = TextureUsage::Sampled });
    });
}
