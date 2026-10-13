//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerDeathTests.cpp
/// @brief Death tests for the texture barrier rules the validation tracker judges at submission.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;
using test::UpscaleKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    int g_surface = 0;

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from `oldLayout` to `newLayout`, with no access on either
    /// side, so only the layout rules can fail.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = oldLayout, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
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
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
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
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{
            .texture = texture, .range = range, .srcStage = Stage::All, .dstStage = Stage::All, .oldLayout = Layout::Undefined, .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
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
TEST_CASE("VU-rojo-cmd-0070 a cleared range is in neither CopyDst nor General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0070", [] {
        PassKit kit(QueueType::Graphics, true);
        kit.list.clearTexture(kit.target(), {}, {});
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
        const Texture texture = kit.texture();
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        // CopyDst permits the copy, so only the Copy list's rule fails.
        move(kit.list, texture, Layout::Undefined, Layout::CopyDst);
        kit.list.copyBufferToTexture(buffer, kLayout, texture, kRegion);
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
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
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
TEST_CASE("VU-rojo-sync-0006 is reported before the entry rule", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0006", [] {
        PassKit kit(QueueType::Copy, true);
        const Buffer buffer = kit.buffer(BufferUsage::None, 4096);
        // The texture stays in Undefined, which breaks VU-rojo-cmd-0067 as well.
        kit.list.copyBufferToTexture(buffer, kLayout, kit.texture(), kRegion);
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
