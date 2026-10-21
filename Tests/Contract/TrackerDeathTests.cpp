//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerDeathTests.cpp
/// @brief Death tests for the texture barrier rules the validation tracker judges at submission.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/Death.h"
#include "Support/PassKit.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

using namespace rojoRHI;
using test::PassKit;
using test::UpscaleKit;

namespace {
    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, covering every use on both
    /// sides, so only the layout rules can fail.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        test::transition(list, texture, oldLayout, newLayout);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0013 a texture barrier starts from a layout the texture is not in", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0013", [] {
        PassKit kit(QueueType::Graphics, true);
        // A texture created without initial data is in Undefined.
        move(kit.list, kit.texture(), Layout::ShaderRead, Layout::CopyDst);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0014 the first barrier on an acquired image starts from a layout other than Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0014", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        move(list, image, Layout::Present, Layout::ColorAttachment);
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    });
}

namespace {
    /// The region of the 4 by 4 texels every layout test copies.
    constexpr TextureRegion kRegion{ .extent = { 4, 4, 1 } };
    /// The buffer layout of `kRegion`.
    constexpr BufferTextureLayout kLayout{ .rowPitch = 256 };

    //==================================================================================================================
    /// Records the barrier that moves the subresources `range` of `texture` from Undefined to `newLayout`.
    void moveRange(CommandList list, Texture texture, const SubresourceRange& range, Layout newLayout)
    {
        test::transition(list, texture, Layout::Undefined, newLayout, range);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0054 the source of a texture copy is in neither CopySrc nor General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0054", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture source = kit.texture();
        const Texture destination = kit.texture();
        move(kit.list, destination, Layout::Undefined, Layout::CopyDst);
        kit.list.copyTexture(source, kRegion, destination, kRegion);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0054 the destination of a texture copy is in neither CopyDst nor General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0054", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture source = kit.texture();
        const Texture destination = kit.texture();
        move(kit.list, source, Layout::Undefined, Layout::CopySrc);
        kit.list.copyTexture(source, kRegion, destination, kRegion);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0067 the destination of a buffer to texture copy is in neither CopyDst nor General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0067", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        kit.list.copyBufferToTexture(buffer, kLayout, kit.texture(), kRegion);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0069 the source of a texture to buffer copy is in neither CopySrc nor General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0069", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        kit.list.copyTextureToBuffer(kit.texture(), kRegion, buffer, kLayout);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0072 a cleared range is not in CopyDst", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0072", [] {
        PassKit kit(QueueType::Graphics, true);
        kit.list.clearTexture(kit.target(), {}, {});
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0072 a cleared range is in General", "[contract][death]")
{
    const std::string_view ends = test::misplacedClearEndsRecording();
    if (!ends.empty())
    {
        SKIP("the clear never reaches the tracker: " << ends);
    }
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0072", [] {
        PassKit kit(QueueType::Graphics, true);
        // A Storage usage makes the move to General legal natively, so only the tracker judges the clear.
        const Texture target = kit.texture(Format::RGBA8Unorm, TextureUsage::RenderTarget | TextureUsage::Storage);
        move(kit.list, target, Layout::Undefined, Layout::General);
        kit.list.clearTexture(target, {}, {});
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0016 an upscale reads or writes a texture in the wrong layout", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0016", [] {
        UpscaleKit kit(QueueType::Graphics, UpscaleKit::defaultDesc(), true);
        move(kit.kit.list, kit.params.color, Layout::Undefined, Layout::ShaderRead);
        move(kit.kit.list, kit.params.depth, Layout::Undefined, Layout::ShaderRead);
        move(kit.kit.list, kit.params.motion, Layout::Undefined, Layout::ShaderRead);
        // The output stays in Undefined.
        kit.upscale();
        kit.kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0006 a texture used by a Copy list is not in General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0006", [] {
        PassKit kit(QueueType::Copy, true);
        const std::vector<std::byte> texels(static_cast<size_t>(PassKit::kSize) * PassKit::kSize * 4);
        const std::array<TextureData, 1> initial{ TextureData{ .bytes = texels, .bytesPerRow = PassKit::kSize * 4 } };
        const Texture texture = *kit.device.createTexture(
            TextureDesc{
                .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" },
            initial
        );
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        // Initial data leaves the texture in ShaderRead, which permits no copy, and a Copy list takes no barrier out of it.
        kit.list.copyTextureToBuffer(texture, kRegion, buffer, kLayout);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0004 a declared write is made to a texture not in ShaderWrite", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0004", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Storage);
        const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
        kit.list.declareAccess(accesses);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatch(1, 1, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0004 a declared read is made to a texture in CopyDst", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0004", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        move(kit.list, texture, Layout::Undefined, Layout::CopyDst);
        const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
        kit.list.declareAccess(accesses);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatch(1, 1, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0018 an image is presented in a layout other than Present", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0018", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        move(list, image, Layout::Undefined, Layout::ColorAttachment);
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
        device.queue(QueueType::Graphics).present(swapchain);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0006 is reported before the entry rule for a placed texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0006", [] {
        PassKit kit(QueueType::Copy, true);
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        // A Copy list finds only a texture that is neither placed nor Sparse in General. This one is placed in a fresh heap, so
        // no earlier use of its memory reports VU-rojo-sync-0017 first, and it stays in Undefined, which breaks
        // VU-rojo-cmd-0067 as well.
        const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
        const Texture texture = *kit.device.createTexture(
            TextureDesc{
                .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Sampled, .label = "test.placed" },
            {},
            Placement{ .heap = heap, .offset = 0 }
        );
        kit.list.copyBufferToTexture(buffer, kLayout, texture, kRegion);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0054 only mip 1 of layer 1 of the destination is in the wrong layout", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0054", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture source = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 2, 2);
        const Texture destination = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 2, 2);
        moveRange(kit.list, source, {}, Layout::CopySrc);
        moveRange(kit.list, destination, SubresourceRange{ .baseMip = 0, .mipCount = 1 }, Layout::CopyDst);
        moveRange(kit.list, destination, SubresourceRange{ .baseMip = 1, .mipCount = 1, .baseLayer = 0, .layerCount = 1 }, Layout::CopyDst);
        // Mip 1 of layer 1 stays in Undefined.
        kit.list.copyTexture(source, kRegion, destination, TextureRegion{ .mip = 1, .layer = 1, .extent = { 4, 4, 1 } });
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0007 a color attachment is not in ColorAttachment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        // The target stays in Undefined.
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        kit.list.beginRenderPass(test::fullPass(colors));
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0007 a resolve target is not in ColorAttachment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture multisampled = kit.target(Format::RGBA8Unorm, 4);
        const Texture resolve = kit.target();
        move(kit.list, multisampled, Layout::Undefined, Layout::ColorAttachment);
        // The resolve target stays in Undefined.
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = multisampled, .resolve = resolve } };
        kit.list.beginRenderPass(test::fullPass(colors));
        kit.list.endRenderPass();
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0010 a depth attachment is not in DepthStencilWrite", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0010", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture color = kit.target();
        move(kit.list, color, Layout::Undefined, Layout::ColorAttachment);
        // The depth texture stays in Undefined.
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color } };
        RenderPassDesc desc = test::fullPass(colors);
        desc.depth = DepthAttachment{ .texture = kit.depthTarget() };
        kit.list.beginRenderPass(desc);
        kit.list.endRenderPass();
        kit.finish();
    });
}
