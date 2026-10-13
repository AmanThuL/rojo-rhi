//----------------------------------------------------------------------------------------------------------------------
/// @file LifetimeDeathTests.cpp
/// @brief Death tests for objects destroyed while an unsubmitted list references them, and for a list that references a presented image.
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
    /// Records a barrier on `texture` that moves it from `oldLayout` to `newLayout`: a command that names the texture and uses nothing else.
    void move(CommandList list, Texture texture, Layout oldLayout, Layout newLayout)
    {
        test::transition(list, texture, oldLayout, newLayout);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the graphics queue.
    void submit(Device device, CommandList list)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-abi-0003 a buffer is destroyed while an unsubmitted list's root names it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-abi-0003", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer root = kit.buffer(BufferUsage::None);
        kit.list.setPipeline(kit.compute());
        kit.list.setRoot(kit.device.address(root));
        kit.list.dispatch(1, 1, 1);
        kit.device.destroy(root);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-abi-0003 is reported before the address and handle rules", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-abi-0003", [] {
        PassKit kit(QueueType::Compute, true);
        const Buffer buffer = kit.buffer(BufferUsage::IndirectArgs);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.setPipeline(kit.compute());
        kit.list.setRoot(kit.device.address(buffer));
        kit.list.dispatch(1, 1, 1);
        kit.list.dispatchIndirect(kit.device.address(buffer));
        kit.list.copyBuffer(buffer, 0, other, 0, 16);
        kit.device.destroy(buffer);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0019 a buffer is destroyed while an unsubmitted list was passed an address in it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0019", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(kit.device.address(args));
        kit.device.destroy(args);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0019 is reported before the handle rule", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0019", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(kit.device.address(args));
        kit.list.copyBuffer(args, 0, other, 0, 16);
        kit.device.destroy(args);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a buffer is destroyed while an unsubmitted list copies it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer source = kit.buffer(BufferUsage::None);
        const Buffer target = kit.buffer(BufferUsage::None);
        kit.list.copyBuffer(source, 0, target, 0, 16);
        kit.device.destroy(source);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a texture is destroyed while an unsubmitted list names it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        move(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
        kit.device.destroy(texture);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a pipeline is destroyed while an unsubmitted list binds it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Pipeline pipeline = kit.compute();
        kit.list.setPipeline(pipeline);
        kit.device.destroy(pipeline);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 an upscaler is destroyed while an unsubmitted list upscales with it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        UpscaleKit kit(QueueType::Graphics, UpscaleKit::defaultDesc(), true);
        kit.upscale();
        kit.kit.device.destroy(kit.upscaler);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0019 a list recorded before a present is submitted after it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0019", [] {
        Device device = *createDevice(DeviceDesc{ .validation = true });
        Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
        device.beginFrame();
        const Texture image = swapchain.acquire();
        CommandList first = device.acquireCommandList(QueueType::Graphics, "test.first");
        CommandList second = device.acquireCommandList(QueueType::Graphics, "test.second");
        move(first, image, Layout::Undefined, Layout::Present);
        move(second, image, Layout::Present, Layout::ColorAttachment);
        submit(device, first);
        device.queue(QueueType::Graphics).present(swapchain);
        submit(device, second);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-abi-0003 a buffer is destroyed after a dispatch read it as the root and the root moved on", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-abi-0003", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer first = kit.buffer(BufferUsage::None);
        const Buffer second = kit.buffer(BufferUsage::None);
        kit.list.setPipeline(kit.compute());
        kit.list.setRoot(kit.device.address(first));
        kit.list.dispatch(1, 1, 1);
        kit.list.setRoot(kit.device.address(second));
        kit.list.dispatch(1, 1, 1);
        kit.device.destroy(first);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a buffer is destroyed while an unsubmitted list's drawIndexed names it as indices", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer indices = kit.buffer(BufferUsage::Index);
        kit.readyToDraw();
        // No indices are read, so the only reference is the one drawIndexed notes itself.
        kit.list.drawIndexed(indices, IndexType::UInt32, 0, 1);
        kit.device.destroy(indices);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0019 a buffer is destroyed while an unsubmitted list's indirect count address names it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0019", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
        const Buffer count = kit.buffer(BufferUsage::IndirectArgs);
        kit.readyToDraw();
        kit.list.drawIndirect(kit.device.address(args), 1, kit.device.address(count));
        kit.device.destroy(count);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a buffer is destroyed while an unsubmitted list's buffer barrier names it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::None);
        const std::array<BufferBarrier, 1> barriers{ BufferBarrier{
            .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } };
        kit.list.barrier({}, barriers);
        kit.device.destroy(buffer);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0007 a buffer is destroyed while one of two lists that copy it is not submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0007", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer source = kit.buffer(BufferUsage::None);
        const Buffer target = kit.buffer(BufferUsage::None);
        CommandList other = kit.device.acquireCommandList(QueueType::Graphics, "test.other");
        kit.list.copyBuffer(source, 0, target, 0, 16);
        other.copyBuffer(source, 0, target, 16, 16);
        submit(kit.device, kit.list);
        kit.device.destroy(source);
    });
}
