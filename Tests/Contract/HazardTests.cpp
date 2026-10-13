//----------------------------------------------------------------------------------------------------------------------
/// @file HazardTests.cpp
/// @brief Tests that the validation tracker accepts uses the barriers between them order, and uses that need no barrier.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every buffer test writes and reads.
    [[maybe_unused]] constexpr uint64_t kBytes = 256;
    /// The region of the 4 by 4 texels every texture test copies.
    [[maybe_unused]] constexpr TextureRegion kRegion{ .extent = { 4, 4, 1 } };
    /// The buffer layout of `kRegion`.
    [[maybe_unused]] constexpr BufferTextureLayout kLayout{ .rowPitch = 256 };

    /// One side of a barrier.
    struct Scope
    {
        Stage stage = Stage::None;    ///< The stages.
        Access access = Access::None; ///< The accesses.
    };

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    [[maybe_unused]] void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }

    //==================================================================================================================
    /// Records the barrier that orders a copy's write of `buffer` before a copy's read of it.
    [[maybe_unused]] void writeToRead(CommandList list)
    {
        global(list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyRead);
    }

    //==================================================================================================================
    /// Records the barrier that moves every subresource of `texture` from Undefined to `newLayout`, making it visible to
    /// `dstStage` and `dstAccess`.
    [[maybe_unused]] void discardTo(CommandList list, Texture texture, Layout newLayout, Stage dstStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::None,
                                                                      .dstStage = dstStage,
                                                                      .dstAccess = dstAccess,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Ends `list` and submits it alone to the graphics queue of `device`.
    [[maybe_unused]] void submit(Device device, CommandList list)
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a write after a write is ordered through a read and two barriers", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer other = kit.buffer(BufferUsage::None);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    writeToRead(kit.list);
    kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
    // Neither barrier covers both writes; the read between them chains the two.
    global(kit.list, Stage::Copy, Access::CopyRead, Stage::Copy, Access::CopyWrite);
    kit.list.fillBuffer(buffer, 0, kBytes, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a read after a read needs no barrier", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer first = kit.buffer(BufferUsage::None);
    const Buffer second = kit.buffer(BufferUsage::None);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    writeToRead(kit.list);
    kit.list.copyBuffer(buffer, 0, first, 0, kBytes);
    kit.list.copyBuffer(buffer, 0, second, 0, kBytes);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a buffer barrier orders exactly the range it names", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer other = kit.buffer(BufferUsage::None);
    kit.list.fillBuffer(buffer, kBytes, kBytes, 0);
    const std::array<BufferBarrier, 1> barriers{ BufferBarrier{ .buffer = buffer,
                                                                .offset = kBytes,
                                                                .size = kBytes,
                                                                .srcStage = Stage::Copy,
                                                                .srcAccess = Access::CopyWrite,
                                                                .dstStage = Stage::Copy,
                                                                .dstAccess = Access::CopyRead } };
    kit.list.barrier({}, barriers);
    kit.list.copyBuffer(buffer, kBytes, other, 0, kBytes);
    // The bytes before the barrier's range were never written, so reading them needs no barrier either.
    kit.list.copyBuffer(buffer, 0, other, kBytes, kBytes);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a global barrier orders every buffer and texture", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer other = kit.buffer(BufferUsage::None, 8 * kBytes);
    const Texture texture = kit.target();
    discardTo(kit.list, texture, Layout::General, Stage::Copy, Access::CopyWrite);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    kit.list.clearTexture(texture, {}, {});
    writeToRead(kit.list);
    kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
    kit.list.copyTextureToBuffer(texture, kRegion, other, BufferTextureLayout{ .offset = 2 * kBytes, .rowPitch = 256 });
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("the uses one render pass makes of its attachments are not compared with each other", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture multisampled = kit.target(Format::RGBA8Unorm, 4);
    const Texture resolve = kit.target();
    const Texture depth = kit.depthTarget(Format::D32Float, 4);
    const std::array<TextureBarrier, 3> barriers{
        TextureBarrier{ .texture = multisampled,
                        .dstStage = Stage::ColorAttachment,
                        .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::ColorAttachment },
        TextureBarrier{ .texture = resolve,
                        .dstStage = Stage::Resolve,
                        .dstAccess = Access::Resolve,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::ColorAttachment },
        TextureBarrier{ .texture = depth,
                        .dstStage = Stage::DepthStencil,
                        .dstAccess = Access::DepthStencilRead | Access::DepthStencilWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::DepthStencilWrite },
    };
    kit.list.barrier({}, {}, barriers);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = multisampled, .resolve = resolve } };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = DepthAttachment{ .texture = depth };
    const std::array<ColorTarget, 1> targets{ ColorTarget{ .format = Format::RGBA8Unorm, .blend = { .enable = true } } };
    const Pipeline pipeline = kit.graphicsWith(targets, Format::D32Float, DepthStencilState{ .depthTest = true, .depthWrite = true }, 4);
    kit.list.beginRenderPass(desc);
    kit.list.setPipeline(pipeline);
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.draw(3);
    kit.list.draw(3);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a draw or dispatch of no work makes no use, though it consumes the declarations before it", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    // Either declared write would follow the fill with no barrier between, if the command made it.
    const std::array<ResourceAccess, 1> compute{ ResourceAccess{ .buffer = buffer, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
    kit.list.setPipeline(kit.compute());
    kit.list.declareAccess(compute);
    kit.list.dispatch(0, 1, 1);
    kit.readyToDraw();
    const std::array<ResourceAccess, 1> vertex{ ResourceAccess{ .buffer = buffer, .stage = Stage::VertexShader, .access = Access::ShaderWrite } };
    kit.list.declareAccess(vertex);
    kit.list.draw(3, 0);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a pipeline whose stencil write mask is zero does not write stencil", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture color = kit.target();
    const Texture depth = kit.depthTarget(Format::D32FloatS8);
    const std::array<TextureBarrier, 2> barriers{
        TextureBarrier{ .texture = color,
                        .dstStage = Stage::ColorAttachment,
                        .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::ColorAttachment },
        // Only reads of the depth-stencil attachment are made visible: a stencil write would be unordered.
        TextureBarrier{ .texture = depth,
                        .dstStage = Stage::DepthStencil,
                        .dstAccess = Access::DepthStencilRead,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::DepthStencilWrite },
    };
    kit.list.barrier({}, {}, barriers);
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color } };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = DepthAttachment{ .texture = depth, .stencilLoad = LoadOp::Load, .stencilStore = StoreOp::Store };
    const std::array<ColorTarget, 1> targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const StencilFaceState replace{ .pass = StencilOp::Replace };
    const DepthStencilState state{ .depthTest = true, .stencilTest = true, .stencilWriteMask = 0, .front = replace, .back = replace };
    kit.list.beginRenderPass(desc);
    kit.list.setPipeline(kit.graphicsWith(targets, Format::D32FloatS8, state));
    kit.list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.draw(3);
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a buffer destroyed and created again in its slot starts with no hazard", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    Device device = *createDevice(DeviceDesc{ .validation = true, .framesInFlight = 2 });
    const BufferDesc desc{ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "test.buffer" };
    const Buffer first = *device.createBuffer(desc);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.first");
    list.fillBuffer(first, 0, kBytes, 0);
    submit(device, list);
    device.destroy(first);
    device.endFrame();
    // The frame after next retires the slot, so the new buffer may take it; its first write follows nothing.
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const Buffer second = *device.createBuffer(desc);
    list = device.acquireCommandList(QueueType::Graphics, "test.second");
    list.fillBuffer(second, 0, kBytes, 1);
    submit(device, list);
    device.endFrame();
    destroyDevice(device);
#endif
}

//======================================================================================================================
TEST_CASE("hazards are judged in queue submission order, not in the order lists were recorded", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer other = kit.buffer(BufferUsage::None);
    // Recorded first, submitted second: in recording order the fill would follow the read with no barrier.
    kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
    kit.list.end();
    CommandList earlier = kit.device.acquireCommandList(QueueType::Graphics, "test.earlier");
    earlier.fillBuffer(buffer, 0, kBytes, 0);
    writeToRead(earlier);
    earlier.end();
    const std::array<CommandList, 2> lists{ earlier, kit.list };
    kit.device.queue(QueueType::Graphics).submit(lists);
    kit.device.endFrame();
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("uses of frame data are not tracked", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const DispatchIndirectArgs args{ 1, 1, 1 };
    const Addr address = kit.list.uploadFrameData(std::as_bytes(std::span(&args, 1)), 256);
    kit.list.setPipeline(kit.compute());
    kit.list.dispatchIndirect(address);
    // The CPU wrote the arguments; the GPU only reads them, within the frame.
    kit.list.dispatchIndirect(kit.list.uploadFrameData(std::as_bytes(std::span(&args, 1)), 256));
    kit.finish();
#endif
}

namespace {
    //==================================================================================================================
    /// Records one texture barrier of every subresource of `texture`.
    [[maybe_unused]] void textureBarrier(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Scope source, Scope destination)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = source.stage,
                                                                      .srcAccess = source.access,
                                                                      .dstStage = destination.stage,
                                                                      .dstAccess = destination.access,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Declares `access` of every subresource of `texture` at `stage` for the next draw or dispatch.
    [[maybe_unused]] void declare(CommandList list, Texture texture, Stage stage, Access access)
    {
        const std::array<ResourceAccess, 1> accesses{ ResourceAccess{ .texture = texture, .stage = stage, .access = access } };
        list.declareAccess(accesses);
    }

    //==================================================================================================================
    /// Records a pass that tests and writes the depth attachment `depth`, whose layout is already `DepthStencilWrite`.
    [[maybe_unused]] void depthPass(PassKit& kit, CommandList list, Texture depth)
    {
        const Texture color = kit.target();
        textureBarrier(list, color, Layout::Undefined, Layout::ColorAttachment, {}, { Stage::ColorAttachment, Access::ColorAttachmentWrite });
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color, .load = LoadOp::Clear } };
        RenderPassDesc desc = test::fullPass(colors);
        desc.depth = DepthAttachment{ .texture = depth };
        const std::array<ColorTarget, 1> targets{ ColorTarget{ .format = Format::RGBA8Unorm } };
        list.beginRenderPass(desc);
        list.setPipeline(kit.graphicsWith(targets, Format::D32Float, DepthStencilState{ .depthTest = true, .depthWrite = true }));
        list.setViewport({ .width = static_cast<float>(PassKit::kSize), .height = static_cast<float>(PassKit::kSize) });
        list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
        list.draw(3);
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Begins a pass, draws once with a fragment read of `texture` declared and ends the pass.
    [[maybe_unused]] void fragmentRead(PassKit& kit, Texture texture)
    {
        kit.readyToDraw();
        declare(kit.list, texture, Stage::FragmentShader, Access::ShaderRead);
        kit.list.draw(3);
        kit.list.endRenderPass();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a depth prepass is read by fragment shaders after a barrier from its depth writes", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    constexpr Scope kDepthWrite{ Stage::DepthStencil, Access::DepthStencilWrite };
    constexpr Scope kFragmentRead{ Stage::FragmentShader, Access::ShaderRead };
    for (const bool transition : { true, false })
    {
        PassKit kit(QueueType::Graphics, true);
        const Texture depth = kit.texture(Format::D32Float, TextureUsage::DepthStencil | TextureUsage::Sampled);
        textureBarrier(
            kit.list, depth, Layout::Undefined, Layout::DepthStencilWrite, {}, { Stage::DepthStencil, Access::DepthStencilRead | Access::DepthStencilWrite }
        );
        depthPass(kit, kit.list, depth);
        if (transition)
        {
            // The barrier orders the depth writes before the reads and moves the texture in one record.
            textureBarrier(kit.list, depth, Layout::DepthStencilWrite, Layout::DepthStencilRead, kDepthWrite, kFragmentRead);
        }
        else
        {
            // A global barrier orders the depth writes before fragment work; the layout change then waits for fragment work.
            global(kit.list, kDepthWrite.stage, kDepthWrite.access, kFragmentRead.stage, kFragmentRead.access);
            textureBarrier(kit.list, depth, Layout::DepthStencilWrite, Layout::DepthStencilRead, { Stage::FragmentShader, Access::None }, kFragmentRead);
        }
        fragmentRead(kit, depth);
        kit.finish();
    }
#endif
}

//======================================================================================================================
TEST_CASE("a colour pass that loads its attachment follows a barrier from the earlier pass's writes", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture color = kit.target();
    textureBarrier(
        kit.list, color, Layout::Undefined, Layout::ColorAttachment, {}, { Stage::ColorAttachment, Access::ColorAttachmentRead | Access::ColorAttachmentWrite }
    );
    const std::array<ColorAttachment, 1> load{ ColorAttachment{ .texture = color, .load = LoadOp::Load } };
    kit.list.beginRenderPass(test::fullPass(load));
    kit.list.endRenderPass();
    // The earlier pass loaded and stored; only its write needs making available. The later pass loads and stores again.
    textureBarrier(
        kit.list,
        color,
        Layout::ColorAttachment,
        Layout::ColorAttachment,
        { Stage::ColorAttachment, Access::ColorAttachmentWrite },
        { Stage::ColorAttachment, Access::ColorAttachmentRead | Access::ColorAttachmentWrite }
    );
    kit.list.beginRenderPass(test::fullPass(load));
    kit.list.endRenderPass();
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a dispatch that reads and writes storage follows a barrier from the earlier shader writes", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const std::array<ResourceAccess, 1> both{ ResourceAccess{
        .buffer = buffer, .stage = Stage::ComputeShader, .access = Access::ShaderRead | Access::ShaderWrite } };
    kit.list.setPipeline(kit.compute());
    kit.list.declareAccess(both);
    kit.list.dispatch(1, 1, 1);
    global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::ComputeShader, Access::ShaderRead | Access::ShaderWrite);
    kit.list.declareAccess(both);
    kit.list.dispatch(1, 1, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a discard follows a read with only the read's stage as its source", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None);
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::ShaderRead, {}, { Stage::ComputeShader, Access::ShaderRead });
    kit.list.setPipeline(kit.compute());
    declare(kit.list, texture, Stage::ComputeShader, Access::ShaderRead);
    kit.list.dispatch(1, 1, 1);
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, { Stage::ComputeShader, Access::None }, { Stage::Copy, Access::CopyWrite });
    kit.list.copyBufferToTexture(buffer, kLayout, texture, kRegion);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a discard follows a write with only the write's stage as its source", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::ShaderWrite, {}, { Stage::ComputeShader, Access::ShaderWrite });
    kit.list.setPipeline(kit.compute());
    declare(kit.list, texture, Stage::ComputeShader, Access::ShaderWrite);
    kit.list.dispatch(1, 1, 1);
    textureBarrier(
        kit.list, texture, Layout::Undefined, Layout::ShaderRead, { Stage::ComputeShader, Access::None }, { Stage::ComputeShader, Access::ShaderRead }
    );
    declare(kit.list, texture, Stage::ComputeShader, Access::ShaderRead);
    kit.list.dispatch(1, 1, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a depth buffer written in one frame is discarded in the next", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture depth = kit.depthTarget();
    constexpr Scope kDepth{ Stage::DepthStencil, Access::DepthStencilRead | Access::DepthStencilWrite };
    CommandList list = kit.list;
    for (int frame = 0; frame < 3; ++frame)
    {
        if (frame > 0)
        {
            kit.device.beginFrame();
            list = kit.device.acquireCommandList(QueueType::Graphics, "test.frame");
        }
        textureBarrier(list, depth, Layout::Undefined, Layout::DepthStencilWrite, { Stage::DepthStencil, Access::None }, kDepth);
        depthPass(kit, list, depth);
        submit(kit.device, list);
        kit.device.endFrame();
    }
    destroyDevice(kit.device);
#endif
}

//======================================================================================================================
TEST_CASE("a write after a read follows a barrier whose destination names no access", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
    const Buffer other = kit.buffer(BufferUsage::None);
    kit.list.fillBuffer(buffer, 0, kBytes, 0);
    writeToRead(kit.list);
    kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
    // An execution dependency is all a write after a read needs.
    global(kit.list, Stage::Copy, Access::None, Stage::Copy, Access::None);
    kit.list.fillBuffer(buffer, 0, kBytes, 1);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a transition is ordered after the last write through a read between them", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget);
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::ShaderWrite, {}, { Stage::ComputeShader, Access::ShaderWrite });
    kit.list.setPipeline(kit.compute());
    declare(kit.list, texture, Stage::ComputeShader, Access::ShaderWrite);
    kit.list.dispatch(1, 1, 1);
    global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::FragmentShader, Access::ShaderRead);
    fragmentRead(kit, texture);
    // The source covers the fragment read, which the barrier before it ordered after the compute write.
    textureBarrier(
        kit.list,
        texture,
        Layout::ShaderWrite,
        Layout::ColorAttachment,
        { Stage::FragmentShader, Access::None },
        { Stage::ColorAttachment, Access::ColorAttachmentWrite }
    );
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("the first transition of a fresh texture needs no source stage", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None);
    // Nothing used the texture before, so a source that waits for nothing orders everything there is.
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::CopyDst, {}, { Stage::Copy, Access::CopyWrite });
    kit.list.copyBufferToTexture(buffer, kLayout, texture, kRegion);
    kit.finish();
#endif
}

//======================================================================================================================
TEST_CASE("a transition chains after the previous one through a barrier between them", "[contract]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Texture texture = kit.texture();
    const Buffer buffer = kit.buffer(BufferUsage::None);
    textureBarrier(kit.list, texture, Layout::Undefined, Layout::ShaderRead, {}, { Stage::FragmentShader, Access::ShaderRead });
    // The second transition waits for copies, which the global orders after fragment work and so after the first transition.
    global(kit.list, Stage::FragmentShader, Access::None, Stage::Copy, Access::None);
    textureBarrier(kit.list, texture, Layout::ShaderRead, Layout::CopyDst, { Stage::Copy, Access::None }, { Stage::Copy, Access::CopyWrite });
    kit.list.copyBufferToTexture(buffer, kLayout, texture, kRegion);
    kit.finish();
#endif
}
