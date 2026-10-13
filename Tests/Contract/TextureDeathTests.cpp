//----------------------------------------------------------------------------------------------------------------------
/// @file TextureDeathTests.cpp
/// @brief Death tests for the valid-usage rules of the texture and view entries and for releasing a view slot.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// A texture description that obeys every rule; each case breaks one.
    TextureDesc validDesc()
    {
        return TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::Sampled };
    }

    //==================================================================================================================
    Texture makeTexture(Device device, const TextureDesc& desc)
    {
        return *device.createTexture(desc);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0001 a texture has Format::Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0001", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.format = Format::Undefined;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0002 a texture has a zero extent component", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0002", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.extent = { 0, 4, 1 };
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0003 a Tex1D texture is two texels high", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0003", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.type = TextureType::Tex1D;
        desc.extent = { 4, 2, 1 };
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0004 a cube texture is not square", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0004", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.type = TextureType::Cube;
        desc.extent = { 8, 4, 1 };
        desc.layers = 6;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0005 a Tex2D texture has two layers", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0005", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.layers = 2;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0039 a texture has more layers than the device allows", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0039", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.type = TextureType::Tex2DArray;
        desc.layers = device.capabilities().limits.maxTextureLayers + 1;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0006 a texture has more mips than its extent allows", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0006", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.mips = 8;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0007 a texture has three samples", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0007", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.samples = 3;
        desc.usage = TextureUsage::RenderTarget;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0008 a multisampled texture has two mips", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0008", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.samples = 4;
        desc.mips = 2;
        desc.usage = TextureUsage::RenderTarget;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0009 a texture has no usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0009", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.usage = TextureUsage::None;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0010 DepthStencil is set on a color format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0010", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.usage = TextureUsage::DepthStencil;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0011 a Tex3D texture has DepthStencil usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0011", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.type = TextureType::Tex3D;
        desc.format = Format::D32Float;
        desc.extent = { 8, 8, 8 };
        desc.usage = TextureUsage::DepthStencil;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0012 Sparse is set on a device without sparse resources", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0012", [] {
        Device device = *createDevice();
        core::coreOf(device).capabilities().placementSparse = false;
        TextureDesc desc = validDesc();
        desc.usage = TextureUsage::Sampled | TextureUsage::Sparse;
        (void)device.createTexture(desc);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0013 a row stride is below the packed row", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0013", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.extent = { 4, 4, 1 };
        const std::array<std::byte, 64> bytes = {};
        const TextureData data{ .bytes = bytes, .bytesPerRow = 4 };
        (void)device.createTexture(desc, std::span(&data, 1));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0014 a Tex2D subresource has a slice stride", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0014", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.extent = { 4, 4, 1 };
        const std::array<std::byte, 64> bytes = {};
        const TextureData data{ .bytes = bytes, .bytesPerRow = 16, .bytesPerImage = 64 };
        (void)device.createTexture(desc, std::span(&data, 1));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0015 initial data is smaller than its strides imply", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0015", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.extent = { 4, 4, 1 };
        const std::array<std::byte, 8> bytes = {};
        const TextureData data{ .bytes = bytes, .bytesPerRow = 16 };
        (void)device.createTexture(desc, std::span(&data, 1));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0015 a row stride is so large that the implied size wraps", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0015", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.extent = { 4, 4, 1 };
        const std::array<std::byte, 64> bytes = {};
        const TextureData data{ .bytes = bytes, .bytesPerRow = 1ull << 63 };
        (void)device.createTexture(desc, std::span(&data, 1));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0016 a view range has no mips", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0016", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .range = { .mipCount = 0 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0017 a view range starts past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0017", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .range = { .baseMip = 1, .mipCount = 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0041 a view range with all counts starts past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0041", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .range = { .baseMip = 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0018 initial data has one entry too many", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0018", [] {
        Device device = *createDevice();
        const std::array<TextureData, 2> data = {};
        (void)device.createTexture(validDesc(), data);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0019 initial data is given for a Sparse texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0019", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.usage = TextureUsage::Sampled | TextureUsage::Sparse;
        const std::array<TextureData, 1> data = {};
        (void)device.createTexture(desc, data);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0020 initial data is given for a texture without Sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0020", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.usage = TextureUsage::RenderTarget;
        const std::array<TextureData, 1> data = {};
        (void)device.createTexture(desc, data);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0021 initial data is given for a multisampled texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0021", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.samples = 4;
        desc.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
        const std::array<TextureData, 1> data = {};
        (void)device.createTexture(desc, data);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0040 initial data is given for D32FloatS8", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0040", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.format = Format::D32FloatS8;
        const std::array<TextureData, 1> data = {};
        (void)device.createTexture(desc, data);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0022 a Storage view is made of a Sampled-only texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0022", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0023 a Tex3D view is made of a Tex2D texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0023", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .type = TextureType::Tex3D });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0024 a Tex2D view covers two layers", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0024", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.type = TextureType::Tex2DArray;
        desc.layers = 2;
        const Texture texture = makeTexture(device, desc);
        (void)device.createView(texture, TextureViewDesc{ .type = TextureType::Tex2D });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0025 a view reinterprets the format outside its family", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0025", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, TextureViewDesc{ .format = Format::R8Unorm });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0026 a Storage view covers two mips", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0026", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.mips = 2;
        desc.usage = TextureUsage::Storage;
        const Texture texture = makeTexture(device, desc);
        (void)device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0027 a color view is made of a depth texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0027", [] {
        Device device = *createDevice();
        TextureDesc desc = validDesc();
        desc.format = Format::D32Float;
        const Texture texture = makeTexture(device, desc);
        (void)device.createView(texture, TextureViewDesc{ .aspect = TextureAspect::Color });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0028 a texture is destroyed while a view of it is live", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0028", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device, validDesc());
        (void)device.createView(texture, {});
        device.destroy(texture);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0010 a view index that createView never returned is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0010", [] {
        Device device = *createDevice();
        device.destroy(ViewIndex{ 7 });
    });
}
