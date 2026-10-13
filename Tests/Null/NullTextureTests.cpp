//----------------------------------------------------------------------------------------------------------------------
/// @file NullTextureTests.cpp
/// @brief Tests what only the Null backend promises about textures and views: placement answers and slot numbering.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("the Null device places a texture at 1024 bytes and rounds its size up to 1024", "[null]")
{
    Device device = *createDevice();
    const PlacementRequirements tiny =
        device.placementRequirements(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 1, 1, 1 }, .usage = TextureUsage::Sampled });
    CHECK(tiny.size == 1024);
    CHECK(tiny.alignment == 1024);
    const PlacementRequirements large =
        device.placementRequirements(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::Sampled });
    CHECK(large.size == 64 * 64 * 4);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null placement size sums the mips, layers and samples", "[null]")
{
    Device device = *createDevice();
    const PlacementRequirements chain =
        device.placementRequirements(TextureDesc{ .format = Format::R8Unorm, .extent = { 64, 64, 1 }, .mips = 7, .layers = 1, .usage = TextureUsage::Sampled });
    CHECK(chain.size == (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1 + 1023) / 1024 * 1024);
    const PlacementRequirements array = device.placementRequirements(
        TextureDesc{ .type = TextureType::Tex2DArray, .format = Format::R8Unorm, .extent = { 64, 64, 1 }, .layers = 3, .usage = TextureUsage::Sampled }
    );
    CHECK(array.size == 3 * 64 * 64);
    const PlacementRequirements multisampled =
        device.placementRequirements(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 32, 32, 1 }, .samples = 4, .usage = TextureUsage::RenderTarget });
    CHECK(multisampled.size == 32 * 32 * 4 * 4);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null placement size counts block-compressed blocks and volume slices", "[null]")
{
    Device device = *createDevice();
    const PlacementRequirements blocks =
        device.placementRequirements(TextureDesc{ .format = Format::BC1Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::Sampled });
    CHECK(blocks.size == 16 * 16 * 8);
    const PlacementRequirements volume = device.placementRequirements(
        TextureDesc{ .type = TextureType::Tex3D, .format = Format::R8Unorm, .extent = { 16, 16, 16 }, .usage = TextureUsage::Sampled }
    );
    CHECK(volume.size == 16 * 16 * 16);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null view heap hands out slots from zero", "[null]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    CHECK(std::to_underlying(*device.createView(texture, {})) == 0);
    CHECK(std::to_underlying(*device.createView(texture, {})) == 1);
    destroyDevice(device);
}
