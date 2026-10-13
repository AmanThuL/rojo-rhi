//----------------------------------------------------------------------------------------------------------------------
/// @file CopyTests.cpp
/// @brief Tests copies, fills and clears against chapter 07 and the copy roles of chapter 03.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

namespace {
    constexpr uint64_t kBig = 1 << 20;

    //==================================================================================================================
    TextureRegion box(uint32_t width, uint32_t height, uint32_t depth = 1)
    {
        return TextureRegion{ .extent = { width, height, depth } };
    }
} // namespace

//======================================================================================================================
TEST_CASE("copyBuffer copies between every pair of buffers the copy roles allow", "[contract]")
{
    PassKit kit;
    const Buffer gpu = kit.buffer(BufferUsage::None, 256);
    const Buffer other = kit.buffer(BufferUsage::None, 256);
    const Buffer upload = kit.buffer(BufferUsage::None, 256, Memory::Upload);
    const Buffer readback = kit.buffer(BufferUsage::None, 256, Memory::Readback);
    kit.list.copyBuffer(gpu, 0, other, 0, 256);
    kit.list.copyBuffer(upload, 0, gpu, 0, 256);
    kit.list.copyBuffer(gpu, 0, readback, 0, 256);
    kit.list.copyBuffer(upload, 4, readback, 8, 4);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("copyBuffer within one buffer accepts ranges that touch but do not overlap", "[contract]")
{
    PassKit kit;
    const Buffer gpu = kit.buffer(BufferUsage::None, 256);
    kit.list.copyBuffer(gpu, 0, gpu, 64, 64);
    kit.list.copyBuffer(gpu, 64, gpu, 0, 64);
    kit.list.copyBuffer(gpu, 192, gpu, 0, 64);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("copies are recorded on every queue type's list", "[contract]")
{
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        PassKit kit(type);
        const Buffer a = kit.buffer(BufferUsage::None, kBig);
        const Buffer b = kit.buffer(BufferUsage::None, kBig);
        const Texture texture = kit.texture();
        kit.list.copyBuffer(a, 0, b, 0, 64);
        kit.list.copyBufferToTexture(a, { .rowPitch = 256 }, texture, TextureRegion{ .extent = { 4, 1, 1 } });
        kit.list.copyTextureToBuffer(texture, TextureRegion{ .extent = { 4, 1, 1 } }, b, { .rowPitch = 256 });
        kit.list.copyTexture(texture, box(8, 8), kit.texture(), box(8, 8));
        kit.finish();
    }
}

//======================================================================================================================
TEST_CASE("copyBufferToTexture and copyTextureToBuffer take regions of mips, layers and depth slices", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::None, kBig);
    const Texture layered = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 4, 3);
    const BufferTextureLayout layout{ .offset = 512, .rowPitch = 256, .rowsPerImage = 16 };
    const TextureRegion region{ .mip = 1, .layer = 1, .layers = 2, .offset = { 8, 8, 0 }, .extent = { 16, 16, 1 } };
    kit.list.copyBufferToTexture(buffer, layout, layered, region);
    kit.list.copyTextureToBuffer(layered, region, buffer, layout);
    const Texture volume = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 4 }, .usage = TextureUsage::Sampled, .label = "volume" }
    );
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256, .rowsPerImage = 16 }, volume, box(16, 16, 4));
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a layout reaching exactly the end of the buffer is accepted", "[contract]")
{
    PassKit kit;
    // 4 rows of 256 bytes, the last one holding only 16 bytes of the 4 by 4 texels: 3 * 256 + 16 bytes from offset 512.
    const Buffer buffer = kit.buffer(BufferUsage::None, 512 + 3 * 256 + 16);
    kit.list.copyBufferToTexture(buffer, { .offset = 512, .rowPitch = 256 }, kit.texture(), box(4, 4));
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a block-compressed region follows blocks and may end at the mip's edge", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::None, kBig);
    const Texture bc1 = kit.texture(Format::BC1Unorm, TextureUsage::Sampled, 7);
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, bc1, TextureRegion{ .offset = { 4, 8, 0 }, .extent = { 8, 4, 1 } });
    // Mip 5 is 2 by 2 texels: not a multiple of the block size, but the box reaches the mip's edge.
    kit.list.copyBufferToTexture(buffer, { .rowPitch = 256 }, bc1, TextureRegion{ .mip = 5, .extent = { 2, 2, 1 } });
    kit.finish();
}

//======================================================================================================================
TEST_CASE("copyTexture copies between textures of one format or of sRGB partners", "[contract]")
{
    PassKit kit;
    const Texture linear = kit.texture(Format::RGBA8Unorm);
    const Texture srgb = kit.texture(Format::RGBA8Unorm_sRGB);
    const Texture layered = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 1, 4);
    kit.list.copyTexture(linear, box(64, 64), srgb, box(64, 64));
    kit.list.copyTexture(srgb, box(16, 16), linear, box(16, 16));
    kit.list.copyTexture(
        layered, TextureRegion{ .layers = 2, .extent = { 8, 8, 1 } }, layered, TextureRegion{ .layer = 2, .layers = 2, .extent = { 8, 8, 1 } }
    );
    kit.list.copyTexture(
        layered, TextureRegion{ .layer = 1, .extent = { 8, 8, 1 } }, layered, TextureRegion{ .layer = 3, .offset = { 8, 8, 0 }, .extent = { 8, 8, 1 } }
    );
    const Texture bc1 = kit.texture(Format::BC1Unorm);
    const Texture bc1Srgb = kit.texture(Format::BC1Unorm_sRGB);
    kit.list.copyTexture(bc1, box(16, 8), bc1Srgb, box(16, 8));
    kit.finish();
}

//======================================================================================================================
TEST_CASE("copyTexture accepts regions of different mips of one texture", "[contract]")
{
    PassKit kit;
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 3);
    kit.list.copyTexture(texture, box(8, 8), texture, TextureRegion{ .mip = 1, .extent = { 8, 8, 1 } });
    kit.finish();
}

//======================================================================================================================
TEST_CASE("fillBuffer fills a ShaderWrite buffer on a Graphics and a Compute list", "[contract]")
{
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute })
    {
        PassKit kit(type);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, 256);
        kit.list.fillBuffer(buffer, 0, 256, 0xDEADBEEFu);
        kit.list.fillBuffer(buffer, 4, 4, 0);
        kit.finish();
    }
}

//======================================================================================================================
TEST_CASE("clearTexture clears color, depth-stencil and storage textures", "[contract]")
{
    PassKit kit;
    kit.list.clearTexture(kit.target(Format::RGBA8Unorm, 1, 3, 2), {}, { .color = { 1, 0, 0, 1 } });
    kit.list.clearTexture(kit.target(), SubresourceRange{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 }, {});
    kit.list.clearTexture(kit.depthTarget(), {}, { .depth = 1.0f });
    kit.list.clearTexture(kit.depthTarget(Format::D32FloatS8), {}, { .depth = 0.0f, .stencil = 255 });
    kit.list.clearTexture(kit.texture(Format::RGBA16Float, TextureUsage::Storage), {}, {});
    kit.finish();
}

//======================================================================================================================
TEST_CASE("clearTexture on a Compute list clears a Storage texture", "[contract]")
{
    PassKit kit(QueueType::Compute);
    kit.list.clearTexture(kit.texture(Format::RGBA8Unorm, TextureUsage::Storage), {}, {});
    kit.finish();
}

//======================================================================================================================
TEST_CASE("copies, fills and clears record again after a render pass", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite, 256);
    kit.beginSimplePass();
    kit.list.endRenderPass();
    kit.list.fillBuffer(buffer, 0, 16, 1);
    kit.list.copyBuffer(buffer, 0, kit.buffer(BufferUsage::None, 256), 0, 16);
    kit.finish();
}
