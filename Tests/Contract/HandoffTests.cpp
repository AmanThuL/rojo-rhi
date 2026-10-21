//----------------------------------------------------------------------------------------------------------------------
/// @file HandoffTests.cpp
/// @brief Tests that the validation tracker accepts textures handed between queues in a layout both queues accept.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The first row of a `PassKit::kSize` square texture, which every copy names.
    [[maybe_unused]] constexpr TextureRegion kRow{ .extent = { PassKit::kSize, 1, 1 } };

    //==================================================================================================================
    /// Ends `list` and submits it alone to the queue of `type` of `device`, waiting on `wait` and signalling `signal`.
    [[maybe_unused]] void submitTo(
        Device device, CommandList list, QueueType type, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {}
    )
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists, wait, signal);
    }

    //==================================================================================================================
    /// Records a dispatch in `list` that declares `access` of every subresource of `texture` from the compute shader.
    [[maybe_unused]] void dispatchWith(PassKit& kit, CommandList list, Texture texture, Access access)
    {
        const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = access } };
        list.declareAccess(accesses);
        list.setPipeline(kit.compute());
        list.dispatch(1, 1, 1);
    }

    //==================================================================================================================
    /// Joins the compute and copy work into the frame's graphics work, ends the frame and destroys the device of `kit`.
    [[maybe_unused]] void close(PassKit& kit)
    {
        kit.joinQueues();
        kit.device.endFrame();
        destroyDevice(kit.device);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a texture handed from Graphics to Compute in ShaderRead is read there with no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    dispatchWith(kit, compute, texture, Access::ShaderRead);
    submitTo(kit.device, compute, QueueType::Compute, moved);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a texture created with data and sampled on Graphics is read on Compute with no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const std::vector<std::byte> texels(static_cast<size_t>(PassKit::kSize) * PassKit::kSize * 4);
    const std::array<TextureData, 1> initial{ TextureData{ .bytes = texels, .bytesPerRow = PassKit::kSize * 4 } };
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { PassKit::kSize, PassKit::kSize, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" },
        initial
    );
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> sampled{ FenceValue{ fence, 1 } };
    // The texture starts in ShaderRead, which the Graphics and Compute queues share, so neither queue needs a barrier.
    dispatchWith(kit, kit.list, texture, Access::ShaderRead);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, sampled);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    dispatchWith(kit, compute, texture, Access::ShaderRead);
    submitTo(kit.device, compute, QueueType::Compute, sampled);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a texture handed from Graphics to Compute in ShaderWrite is moved there from ShaderWrite", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 1 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderWrite);
    dispatchWith(kit, kit.list, texture, Access::ShaderWrite);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, written);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    test::transition(compute, texture, Layout::ShaderWrite, Layout::ShaderRead);
    dispatchWith(kit, compute, texture, Access::ShaderRead);
    submitTo(kit.device, compute, QueueType::Compute, written);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a new texture uploaded on Copy is read on Graphics in General and then moved from General", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Buffer staging = kit.buffer(BufferUsage::None, 64 * 1024);
    const Buffer readback = kit.buffer(BufferUsage::ShaderWrite, 64 * 1024);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> uploaded{ FenceValue{ fence, 1 } };
    // A Copy list finds a new texture in General, so it uploads with no barrier and hands the texture over in General.
    CommandList copy = kit.device.acquireCommandList(QueueType::Copy, "test.copy");
    copy.copyBufferToTexture(staging, BufferTextureLayout{ .rowPitch = 256 }, texture, kRow);
    submitTo(kit.device, copy, QueueType::Copy, {}, uploaded);
    kit.list.copyTextureToBuffer(texture, kRow, readback, BufferTextureLayout{ .rowPitch = 256 });
    test::transition(kit.list, texture, Layout::General, Layout::ShaderRead);
    dispatchWith(kit, kit.list, texture, Access::ShaderRead);
    submitTo(kit.device, kit.list, QueueType::Graphics, uploaded);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("Compute discards a texture handed over from Graphics before writing it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> read{ FenceValue{ fence, 1 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderRead);
    dispatchWith(kit, kit.list, texture, Access::ShaderRead);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, read);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    test::transition(compute, texture, Layout::Undefined, Layout::ShaderWrite);
    dispatchWith(kit, compute, texture, Access::ShaderWrite);
    submitTo(kit.device, compute, QueueType::Compute, read);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a texture handed to Compute and back to Graphics follows the handoff rules both ways", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> moved{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> written{ FenceValue{ fence, 2 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ShaderWrite);
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, moved);
    // Compute writes in the layout Graphics left, with no barrier of its own.
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    dispatchWith(kit, compute, texture, Access::ShaderWrite);
    submitTo(kit.device, compute, QueueType::Compute, moved, written);
    // Graphics takes it back: its first barrier starts from the layout Compute handed over.
    CommandList back = kit.device.acquireCommandList(QueueType::Graphics, "test.back");
    test::transition(back, texture, Layout::ShaderWrite, Layout::ShaderRead);
    dispatchWith(kit, back, texture, Access::ShaderRead);
    submitTo(kit.device, back, QueueType::Graphics, written);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("a swapchain image handed to Compute in General and back is presented", "[contract]")
{
#ifndef NDEBUG
    Device device = *createDevice(DeviceDesc{ .validation = true });
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = test::surface(), .label = "test.swapchain" });
    const Fence fence = *device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> drawn{ FenceValue{ fence, 1 } };
    const std::array<FenceValue, 1> handled{ FenceValue{ fence, 2 } };
    device.beginFrame();
    const Texture image = swapchain.acquire();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    test::transition(graphics, image, Layout::Undefined, Layout::ColorAttachment);
    test::transition(graphics, image, Layout::ColorAttachment, Layout::General);
    submitTo(device, graphics, QueueType::Graphics, {}, drawn);
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    test::transition(compute, image, Layout::General, Layout::General);
    submitTo(device, compute, QueueType::Compute, drawn, handled);
    CommandList back = device.acquireCommandList(QueueType::Graphics, "test.back");
    test::transition(back, image, Layout::General, Layout::Present);
    submitTo(device, back, QueueType::Graphics, handled);
    device.queue(QueueType::Graphics).present(swapchain);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("Compute discards a texture handed over from Graphics in ColorAttachment", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::RenderTarget | TextureUsage::Storage);
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> rendered{ FenceValue{ fence, 1 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ColorAttachment);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture } };
    kit.list.beginRenderPass(test::fullPass(colors));
    kit.list.endRenderPass();
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, rendered);
    // A discard takes nothing over, so the layout Graphics left need not be one Compute accepts.
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    test::transition(compute, texture, Layout::Undefined, Layout::ShaderWrite);
    dispatchWith(kit, compute, texture, Access::ShaderWrite);
    submitTo(kit.device, compute, QueueType::Compute, rendered);
    close(kit);
#endif
}

//======================================================================================================================
TEST_CASE("Compute discards a placed render target after a buffer used its memory there", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = 1u << 20, .label = "test.heap" });
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm,
                     .extent = { PassKit::kSize, PassKit::kSize, 1 },
                     .usage = TextureUsage::RenderTarget | TextureUsage::Storage,
                     .label = "test.placed.texture" },
        {},
        Placement{ .heap = heap }
    );
    const Buffer buffer =
        *kit.device.createBuffer(BufferDesc{ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.placed.buffer" }, {}, Placement{ .heap = heap });
    const Fence fence = *kit.device.createFence(0, "test.fence");
    const std::array<FenceValue, 1> rendered{ FenceValue{ fence, 1 } };
    test::transition(kit.list, texture, Layout::Undefined, Layout::ColorAttachment);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = texture } };
    kit.list.beginRenderPass(test::fullPass(colors));
    kit.list.endRenderPass();
    submitTo(kit.device, kit.list, QueueType::Graphics, {}, rendered);
    CommandList compute = kit.device.acquireCommandList(QueueType::Compute, "test.compute");
    const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ .srcStage = Stage::All, .dstStage = Stage::All, .dstAccess = Access::CopyWrite } };
    compute.barrier(globals);
    compute.fillBuffer(buffer, 0, 256, 7);
    test::transition(compute, texture, Layout::Undefined, Layout::ShaderWrite);
    dispatchWith(kit, compute, texture, Access::ShaderWrite);
    submitTo(kit.device, compute, QueueType::Compute, rendered);
    close(kit);
#endif
}
