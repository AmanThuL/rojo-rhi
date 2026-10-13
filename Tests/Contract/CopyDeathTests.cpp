//----------------------------------------------------------------------------------------------------------------------
/// @file CopyDeathTests.cpp
/// @brief Death tests for the valid-usage rules of copies, fills and clears and the copy roles of buffers.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

namespace {
    constexpr uint64_t kBig = 1 << 20;
    constexpr BufferTextureLayout kLayout{ .rowPitch = 256 };

    //==================================================================================================================
    TextureRegion box(uint32_t width, uint32_t height)
    {
        return TextureRegion{ .extent = { width, height, 1 } };
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0001 copyBuffer writes an Upload buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0001", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None), 0, kit.buffer(BufferUsage::None, 4096, Memory::Upload), 0, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0002 copyBuffer reads a Readback buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0002", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None, 4096, Memory::Readback), 0, kit.buffer(BufferUsage::None), 0, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0034 a layout offset is not a multiple of 512", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0034", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), { .offset = 256, .rowPitch = 256 }, kit.texture(), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0035 a layout row pitch is not a multiple of 256", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0035", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), { .rowPitch = 128 }, kit.texture(), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0036 a layout has fewer rows per image than the region", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0036", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), { .rowPitch = 256, .rowsPerImage = 8 }, kit.texture(), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0037 images of a layout are not a multiple of 512 bytes apart", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0037", [] {
        PassKit kit;
        const Texture layered = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 2);
        kit.list.copyBufferToTexture(
            kit.buffer(BufferUsage::None, kBig), { .rowPitch = 256, .rowsPerImage = 17 }, layered, TextureRegion{ .layers = 2, .extent = { 16, 16, 1 } }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0038 a region names a mip the texture lacks", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0038", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), kLayout, kit.texture(), TextureRegion{ .mip = 1, .extent = { 16, 16, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0039 a region has no texels", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0039", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), kLayout, kit.texture(), box(0, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0040 a block-compressed region cuts a block", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0040", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), kLayout, kit.texture(Format::BC1Unorm), box(6, 8));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0041 a region names a texture with a stencil component", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0041", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), kLayout, kit.depthTarget(Format::D32FloatS8), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0042 copyBuffer is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0042", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::None);
        kit.beginSimplePass();
        kit.list.copyBuffer(buffer, 0, kit.buffer(BufferUsage::None), 0, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0043 copyBuffer copies no bytes", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0043", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None), 0, kit.buffer(BufferUsage::None), 0, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0043 copyBuffer range end wraps past 2^64", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0043", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None), ~uint64_t{ 0 } - 3, kit.buffer(BufferUsage::None), 0, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0044 copyBuffer copies a range onto itself", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0044", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::None);
        kit.list.copyBuffer(buffer, 0, buffer, 32, 64);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0046 copyBufferToTexture is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0046", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::None, kBig);
        const Texture texture = kit.texture();
        kit.beginSimplePass();
        kit.list.copyBufferToTexture(buffer, kLayout, texture, box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0049 copyTextureToBuffer is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0049", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::None, kBig);
        const Texture texture = kit.texture();
        kit.beginSimplePass();
        kit.list.copyTextureToBuffer(texture, box(16, 16), buffer, kLayout);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0052 copyTexture is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0052", [] {
        PassKit kit;
        const Texture a = kit.texture();
        const Texture b = kit.texture();
        kit.beginSimplePass();
        kit.list.copyTexture(a, box(16, 16), b, box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0053 copyTexture copies between formats that are not copy-compatible", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0053", [] {
        PassKit kit;
        kit.list.copyTexture(kit.texture(Format::RGBA8Unorm), box(16, 16), kit.texture(Format::BGRA8Unorm), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0055 copyTexture copies regions that share a subresource", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0055", [] {
        PassKit kit;
        const Texture texture = kit.texture();
        kit.list.copyTexture(texture, box(16, 16), texture, TextureRegion{ .offset = { 32, 32, 0 }, .extent = { 16, 16, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0056 fillBuffer is recorded on a Copy list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0056", [] {
        PassKit kit(QueueType::Copy);
        kit.list.fillBuffer(kit.buffer(BufferUsage::ShaderWrite), 0, 16, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0057 fillBuffer fills no bytes", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0057", [] {
        PassKit kit;
        kit.list.fillBuffer(kit.buffer(BufferUsage::ShaderWrite), 0, 0, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0058 fillBuffer fills a buffer without ShaderWrite", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0058", [] {
        PassKit kit;
        kit.list.fillBuffer(kit.buffer(BufferUsage::None), 0, 16, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0059 clearTexture clears to a depth above one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0059", [] {
        PassKit kit;
        kit.list.clearTexture(kit.depthTarget(), {}, { .depth = 2.0f });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0060 clearTexture is recorded on a Copy list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0060", [] {
        PassKit kit(QueueType::Copy);
        kit.list.clearTexture(kit.target(), {}, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0061 clearTexture clears a texture that is neither attachment nor storage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0061", [] {
        PassKit kit;
        kit.list.clearTexture(kit.texture(), {}, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0064 copyBufferToTexture reads past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0064", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, 1024), kLayout, kit.texture(), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0064 copyBufferToTexture addresses bytes past 2^64", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0064", [] {
        PassKit kit;
        const Texture layered = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 4);
        kit.list.copyBufferToTexture(
            kit.buffer(BufferUsage::None, kBig),
            { .rowPitch = 0xFFFFFF00u, .rowsPerImage = 0xFFFFFFFEu },
            layered,
            TextureRegion{ .layers = 4, .extent = { 16, 16, 1 } }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0065 copyTextureToBuffer writes past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0065", [] {
        PassKit kit;
        kit.list.copyTextureToBuffer(kit.texture(), box(16, 16), kit.buffer(BufferUsage::None, 1024), kLayout);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0066 copyBufferToTexture writes a multisampled texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0066", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig), kLayout, kit.target(Format::RGBA8Unorm, 4), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0068 copyTextureToBuffer reads a multisampled texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0068", [] {
        PassKit kit;
        kit.list.copyTextureToBuffer(kit.target(Format::RGBA8Unorm, 4), box(16, 16), kit.buffer(BufferUsage::None, kBig), kLayout);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0002 copyBufferToTexture reads a Readback buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0002", [] {
        PassKit kit;
        kit.list.copyBufferToTexture(kit.buffer(BufferUsage::None, kBig, Memory::Readback), kLayout, kit.texture(), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0001 copyTextureToBuffer writes an Upload buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0001", [] {
        PassKit kit;
        kit.list.copyTextureToBuffer(kit.texture(), box(16, 16), kit.buffer(BufferUsage::None, kBig, Memory::Upload), kLayout);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0043 copyBuffer starts at an offset that is not a multiple of 4", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0043", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None), 2, kit.buffer(BufferUsage::None), 0, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0043 copyBuffer writes past the end of the destination", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0043", [] {
        PassKit kit;
        kit.list.copyBuffer(kit.buffer(BufferUsage::None), 0, kit.buffer(BufferUsage::None, 4096), 4088, 16);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0053 copyTexture copies between multisampled textures", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0053", [] {
        PassKit kit;
        kit.list.copyTexture(kit.target(Format::RGBA8Unorm, 4), box(16, 16), kit.target(Format::RGBA8Unorm, 4), box(16, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0053 copyTexture copies between regions of different extents", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0053", [] {
        PassKit kit;
        kit.list.copyTexture(kit.texture(), box(16, 16), kit.texture(), box(8, 16));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0053 copyTexture copies between regions of different layer counts", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0053", [] {
        PassKit kit;
        const Texture layered = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 2);
        kit.list.copyTexture(
            layered, box(16, 16), kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 2), TextureRegion{ .layers = 2, .extent = { 16, 16, 1 } }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0056 fillBuffer is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0056", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        kit.beginSimplePass();
        kit.list.fillBuffer(buffer, 0, 16, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0057 fillBuffer fills at an offset that is not a multiple of 4", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0057", [] {
        PassKit kit;
        kit.list.fillBuffer(kit.buffer(BufferUsage::ShaderWrite), 2, 16, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0057 fillBuffer fills past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0057", [] {
        PassKit kit;
        kit.list.fillBuffer(kit.buffer(BufferUsage::ShaderWrite, 4096), 4088, 16, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0060 clearTexture is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0060", [] {
        PassKit kit;
        const Texture texture = kit.target();
        kit.beginSimplePass();
        kit.list.clearTexture(texture, {}, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0061 clearTexture on a Compute list clears a render target", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0061", [] {
        PassKit kit(QueueType::Compute);
        kit.list.clearTexture(kit.target(), {}, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0016 a clear range has no layers", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0016", [] {
        PassKit kit;
        kit.list.clearTexture(kit.target(), { .layerCount = 0 }, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0017 a clear range ends past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0017", [] {
        PassKit kit;
        kit.list.clearTexture(kit.target(), { .baseMip = 1, .mipCount = 1 }, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0041 a clear range with all counts starts past the last layer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0041", [] {
        PassKit kit;
        kit.list.clearTexture(kit.target(), { .baseLayer = 1 }, {});
    });
}
