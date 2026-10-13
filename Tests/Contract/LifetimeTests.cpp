//----------------------------------------------------------------------------------------------------------------------
/// @file LifetimeTests.cpp
/// @brief Tests that objects are destroyed freely once the lists that reference them are submitted.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// A stand-in for a native surface: the Null backend never dereferences it.
    [[maybe_unused]] int g_surface = 0;

    //==================================================================================================================
    /// Records, on `list`, one command of each kind that references an object: a root, an address, a handle and a pipeline.
    [[maybe_unused]] void referenceEverything(PassKit& kit, Buffer root, Buffer args, Buffer source, Buffer target)
    {
        kit.list.setPipeline(kit.compute());
        kit.list.setRoot(kit.device.address(root));
        kit.list.dispatchIndirect(kit.device.address(args));
        kit.list.copyBuffer(source, 0, target, 0, 16);
    }
} // namespace

//======================================================================================================================
TEST_CASE("objects are destroyed after the list that referenced them is submitted", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer root = kit.buffer(BufferUsage::None);
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer target = kit.buffer(BufferUsage::None);
    referenceEverything(kit, root, args, source, target);
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    for (const Buffer buffer : { root, args, source, target })
    {
        kit.device.destroy(buffer);
    }
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("objects a list submitted in an earlier frame referenced are destroyed in a later one", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer target = kit.buffer(BufferUsage::None);
    const Pipeline pipeline = kit.compute();
    kit.list.setPipeline(pipeline);
    kit.list.copyBuffer(source, 0, target, 0, 16);
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    kit.device.endFrame();
    kit.device.beginFrame();
    kit.device.destroy(source);
    kit.device.destroy(pipeline);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a list that references an object twice releases it with one submission", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(kit.device.address(args));
    kit.list.dispatchIndirect(kit.device.address(args));
    kit.list.end();
    const std::array<CommandList, 1> lists{ kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    kit.device.destroy(args);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a buffer a root named and no dispatch read is destroyed before the submission", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer first = kit.buffer(BufferUsage::None);
    const Buffer second = kit.buffer(BufferUsage::None);
    kit.list.setPipeline(kit.compute());
    kit.list.setRoot(kit.device.address(first));
    kit.list.setRoot(kit.device.address(second));
    kit.list.dispatch(1, 1, 1);
    kit.device.destroy(first);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a frame-data root does not hold any buffer", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer other = kit.buffer(BufferUsage::None);
    const std::array<std::byte, 16> bytes{};
    kit.list.setPipeline(kit.compute());
    kit.list.setRoot(kit.list.uploadFrameData(bytes, 256));
    kit.list.dispatch(1, 1, 1);
    kit.device.destroy(other);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a device without validation never reports these rules", "[contract]")
{
    PassKit kit(QueueType::Graphics, false);
    const Buffer args = kit.buffer(BufferUsage::IndirectArgs);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer target = kit.buffer(BufferUsage::None);
    const Pipeline pipeline = kit.compute();
    kit.list.setPipeline(pipeline);
    kit.list.setRoot(kit.device.address(args));
    kit.list.dispatchIndirect(kit.device.address(args));
    kit.list.copyBuffer(source, 0, target, 0, 16);
    kit.device.destroy(args);
    kit.device.destroy(source);
    kit.device.destroy(pipeline);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a recycled list does not keep the swapchain images of its earlier frame", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "test.swapchain" });
    // Each frame's list reuses the slot of the list `framesInFlight` frames before it: the first presented an image, and every
    // later one names none.
    for (uint32_t frame = 0; frame < 2 * DeviceDesc{}.framesInFlight + 1; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.frame");
        const bool presents = frame == 0;
        if (presents)
        {
            const std::array<TextureBarrier, 1> toPresent{ TextureBarrier{
                .texture = swapchain.acquire(), .srcStage = Stage::ColorAttachment, .oldLayout = Layout::Undefined, .newLayout = Layout::Present } };
            list.barrier({}, {}, toPresent);
        }
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
        if (presents)
        {
            device.queue(QueueType::Graphics).present(swapchain);
        }
        device.endFrame();
    }
    device.destroy(swapchain);
    destroyDevice(device);
#endif
}
