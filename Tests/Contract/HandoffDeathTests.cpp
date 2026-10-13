//----------------------------------------------------------------------------------------------------------------------
/// @file HandoffDeathTests.cpp
/// @brief Death tests for textures handed from one queue to another in a layout the two queues do not share.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    int g_surface = 0;

    /// The first row of a `PassKit::kSize` square texture, which every copy names.
    constexpr TextureRegion kRow{ .extent = { PassKit::kSize, 1, 1 } };

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    void submitTo(Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {})
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Records a dispatch in `list` that declares `access` of every subresource of `texture` from the compute shader.
    void dispatchWith(PassKit& kit, CommandList list, Texture texture, Access access)
    {
        const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = access } };
        list.declareAccess(accesses);
        list.setPipeline(kit.compute());
        list.dispatch(1, 1, 1);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0025 a depth texture handed from Graphics to Compute in DepthStencilRead is sampled there", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0025", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture(Format::D32Float, TextureUsage::DepthStencil | TextureUsage::Sampled);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        test::transition(kit.list, texture, Layout::Undefined, Layout::DepthStencilRead);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        // DepthStencilRead permits the sampled read, but the Compute queue does not accept it from Graphics.
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        dispatchWith(kit, compute, texture, Access::ShaderRead);
        submitTo(kit.device, compute, QueueType::Compute, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0025 a texture handed from Graphics to Copy in ShaderRead is copied there", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0025", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const Buffer readback = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        // A handoff to or from the Copy queue is in General; the rule reports before the Copy list's own.
        CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
        copy.copyTextureToBuffer(texture, kRow, readback, BufferTextureLayout{ .rowPitch = 256 });
        submitTo(kit.device, copy, QueueType::Copy, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0025 a texture handed from Copy to Graphics in CopyDst is moved there from CopyDst", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0025", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> uploaded{ FenceValue{ fence, 1 } };
        CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
        test::transition(copy, texture, Layout::Undefined, Layout::CopyDst);
        submitTo(kit.device, copy, QueueType::Copy, {}, uploaded);
        // Graphics starts from the layout the Copy queue left, which only the Copy queue's own rule would have refused.
        test::transition(kit.list, texture, Layout::CopyDst, Layout::ShaderRead);
        submitTo(kit.device, kit.list, QueueType::Graphics, uploaded);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0025 Compute's first barrier on a texture handed over in ShaderRead starts from General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0025", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        test::transition(compute, texture, Layout::General, Layout::ShaderWrite);
        submitTo(kit.device, compute, QueueType::Compute, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0025 Compute's first barrier on a swapchain image handed over in General starts from ShaderRead", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0025", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
        const Fence fence = *device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> drawn{ FenceValue{ fence, 1 } };
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
        test::transition(graphics, image, Layout::Undefined, Layout::ColorAttachment);
        test::transition(graphics, image, Layout::ColorAttachment, Layout::General);
        submitTo(device, graphics, QueueType::Graphics, {}, drawn);
        CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
        test::transition(compute, image, Layout::ShaderRead, Layout::General);
        submitTo(device, compute, QueueType::Compute, drawn);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0005 a texture moved on Graphics after the value Compute waits on is not handed over", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0005", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const Fence fence = *kit.device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
        test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
        submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
        // The second move is not ordered before the Compute read: the race is reported, not the layout either queue sees.
        CommandList again = kit.device.acquireCommandList(QueueType::Graphics, "test.again");
        test::transition(again, texture, Layout::ShaderRead, Layout::CopyDst);
        submitTo(kit.device, again, QueueType::Graphics);
        CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
        dispatchWith(kit, compute, texture, Access::ShaderRead);
        submitTo(kit.device, compute, QueueType::Compute, moved);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0018 an image moved to Present on Copy is presented on Graphics with no fence after the move", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0018", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
        const Fence fence = *device.createFence(0, "test.fence");
        const std::array<FenceValue, 1> drawn{ FenceValue{ fence, 1 } };
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
        test::transition(graphics, image, Layout::Undefined, Layout::ColorAttachment);
        test::transition(graphics, image, Layout::ColorAttachment, Layout::General);
        submitTo(device, graphics, QueueType::Graphics, {}, drawn);
        CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
        test::transition(copy, image, Layout::General, Layout::Present);
        submitTo(device, copy, QueueType::Copy, drawn);
        // The graphics queue never waits on the Copy move, so the image is not in Present there.
        device.queue(QueueType::Graphics).present(swapchain);
    });
}
