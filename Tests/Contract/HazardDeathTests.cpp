//----------------------------------------------------------------------------------------------------------------------
/// @file HazardDeathTests.cpp
/// @brief Death tests for the same-queue hazards and barrier coverage the validation tracker judges at submission.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    /// The bytes every buffer test writes and reads.
    constexpr uint64_t kBytes = 256;
    /// The region of the 4 by 4 texels every texture test copies.
    constexpr TextureRegion kRegion{ .extent = { 4, 4, 1 } };
    /// The buffer layout of `kRegion`.
    constexpr BufferTextureLayout kLayout{ .rowPitch = 256 };

    //==================================================================================================================
    /// Records one global barrier from `srcStage` and `srcAccess` to `dstStage` and `dstAccess`.
    void global(CommandList list, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{
            .srcStage = srcStage, .srcAccess = srcAccess, .dstStage = dstStage, .dstAccess = dstAccess } };
        list.barrier(globals);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0021 a buffer is written twice with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0021", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        kit.list.fillBuffer(buffer, 0, kBytes, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0021 a texture is written after a read with no barrier between", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0021", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.target();
        const Buffer buffer = kit.buffer(BufferUsage::None);
        // The transition orders the read and the clear after it; nothing orders the clear after the read.
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::Copy,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = Access::CopyRead | Access::CopyWrite,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::General } };
        kit.list.barrier({}, {}, barriers);
        kit.list.copyTextureToBuffer(texture, kRegion, buffer, kLayout);
        kit.list.clearTexture(texture, {}, {});
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0021 a buffer barrier on another range leaves the written range without a barrier", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0021", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        const std::array<BufferBarrier, 1> barriers{ BufferBarrier{ .buffer = buffer,
                                                                    .offset = kBytes,
                                                                    .size = kBytes,
                                                                    .srcStage = Stage::Copy,
                                                                    .srcAccess = Access::CopyWrite,
                                                                    .dstStage = Stage::Copy,
                                                                    .dstAccess = Access::CopyRead } };
        kit.list.barrier({}, barriers);
        kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a barrier's source misses the earlier write", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::Copy, Access::CopyRead);
        kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a barrier's destination misses the later use", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderRead);
        kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a transition's source misses an earlier read", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const Buffer buffer = kit.buffer(BufferUsage::None);
        const std::array<TextureBarrier, 1> toSource{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::Copy,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = Access::CopyRead,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::CopySrc } };
        kit.list.barrier({}, {}, toSource);
        kit.list.copyTextureToBuffer(texture, kRegion, buffer, kLayout);
        // The read ran at the Copy stage; the transition waits for compute work only.
        const std::array<TextureBarrier, 1> toDestination{ TextureBarrier{ .texture = texture,
                                                                           .srcStage = Stage::ComputeShader,
                                                                           .dstStage = Stage::Copy,
                                                                           .dstAccess = Access::CopyWrite,
                                                                           .oldLayout = Layout::CopySrc,
                                                                           .newLayout = Layout::CopyDst } };
        kit.list.barrier({}, {}, toDestination);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 only a barrier's source access misses the earlier write", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        // The source stage is right; without CopyWrite the fill is not made available.
        global(kit.list, Stage::Copy, Access::None, Stage::Copy, Access::CopyRead);
        kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 only a barrier's destination access misses a use after a write", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const Buffer other = kit.buffer(BufferUsage::None);
        kit.list.fillBuffer(buffer, 0, kBytes, 0);
        // The destination stage is right; without CopyRead the fill is not made visible to the copy.
        global(kit.list, Stage::Copy, Access::CopyWrite, Stage::Copy, Access::None);
        kit.list.copyBuffer(buffer, 0, other, 0, kBytes);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a barrier after a transition does not chain from the transition's destination stage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .dstStage = Stage::FragmentShader,
                                                                      .dstAccess = Access::ShaderRead,
                                                                      .oldLayout = Layout::Undefined,
                                                                      .newLayout = Layout::ShaderRead } };
        kit.list.barrier({}, {}, barriers);
        // The transition completes before fragment work only; a barrier from the Copy stage does not wait for it.
        global(kit.list, Stage::Copy, Access::None, Stage::ComputeShader, Access::ShaderRead);
        const std::array<ResourceAccess, 1> reads{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
        kit.list.setPipeline(kit.compute());
        kit.list.declareAccess(reads);
        kit.list.dispatch(1, 1, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a transition whose source has no stage follows an earlier read", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const std::array<TextureBarrier, 1> toRead{ TextureBarrier{ .texture = texture,
                                                                    .dstStage = Stage::FragmentShader,
                                                                    .dstAccess = Access::ShaderRead,
                                                                    .oldLayout = Layout::Undefined,
                                                                    .newLayout = Layout::ShaderRead } };
        kit.list.barrier({}, {}, toRead);
        kit.readyToDraw();
        const std::array<ResourceAccess, 1> reads{ ResourceAccess{ .texture = texture, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
        kit.list.declareAccess(reads);
        kit.list.draw(3);
        kit.list.endRenderPass();
        // The barrier covers the read, but the transition waits for nothing, so it does not chain from the barrier.
        global(kit.list, Stage::FragmentShader, Access::None, Stage::ComputeShader, Access::None);
        const std::array<TextureBarrier, 1> toCopy{ TextureBarrier{
            .texture = texture, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite, .oldLayout = Layout::ShaderRead, .newLayout = Layout::CopyDst } };
        kit.list.barrier({}, {}, toCopy);
        kit.finish();
    });
}

namespace {
    //==================================================================================================================
    /// Records, in one barrier call, a global barrier from fragment work to copies and the transition of `texture` from
    /// `oldLayout` to `CopyDst` that waits for copies only: the global would chain the transition after fragment work if
    /// records of one call ordered each other.
    void sameCallChain(CommandList list, Texture texture, Layout oldLayout)
    {
        const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ .srcStage = Stage::FragmentShader, .dstStage = Stage::Copy } };
        const std::array<TextureBarrier, 1> textures{ TextureBarrier{ .texture = texture,
                                                                      .srcStage = Stage::Copy,
                                                                      .dstStage = Stage::Copy,
                                                                      .dstAccess = Access::CopyWrite,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = Layout::CopyDst } };
        list.barrier(globals, {}, textures);
    }

    //==================================================================================================================
    /// Begins a pass, draws once with a fragment read of `texture` declared and ends the pass.
    void fragmentRead(PassKit& kit, Texture texture)
    {
        kit.readyToDraw();
        const std::array<ResourceAccess, 1> reads{ ResourceAccess{ .texture = texture, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
        kit.list.declareAccess(reads);
        kit.list.draw(3);
        kit.list.endRenderPass();
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a global barrier of the same call does not chain a transition after a read", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture();
        const std::array<TextureBarrier, 1> toRead{ TextureBarrier{ .texture = texture,
                                                                    .dstStage = Stage::FragmentShader,
                                                                    .dstAccess = Access::ShaderRead,
                                                                    .oldLayout = Layout::Undefined,
                                                                    .newLayout = Layout::ShaderRead } };
        kit.list.barrier({}, {}, toRead);
        fragmentRead(kit, texture);
        sameCallChain(kit.list, texture, Layout::ShaderRead);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a global barrier of the same call does not chain a transition after the last write through a read", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
        const std::array<TextureBarrier, 1> toWrite{ TextureBarrier{ .texture = texture,
                                                                     .dstStage = Stage::ComputeShader,
                                                                     .dstAccess = Access::ShaderWrite,
                                                                     .oldLayout = Layout::Undefined,
                                                                     .newLayout = Layout::ShaderWrite } };
        kit.list.barrier({}, {}, toWrite);
        const std::array<ResourceAccess, 1> writes{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
        kit.list.setPipeline(kit.compute());
        kit.list.declareAccess(writes);
        kit.list.dispatch(1, 1, 1);
        global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::FragmentShader, Access::ShaderRead);
        // The read is ordered after the write; only the same call's global would order it, and so the write, before the transition.
        fragmentRead(kit, texture);
        sameCallChain(kit.list, texture, Layout::ShaderWrite);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a later barrier whose source shares no stage with the earlier destination does not chain", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);
        const std::array<ResourceAccess, 1> writes{ ResourceAccess{ .buffer = buffer, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
        kit.list.setPipeline(kit.compute());
        kit.list.declareAccess(writes);
        kit.list.dispatch(1, 1, 1);
        global(kit.list, Stage::ComputeShader, Access::ShaderWrite, Stage::IndirectArgs, Access::IndirectArgs);
        // The Copy stage waits for nothing the first barrier's IndirectArgs destination ordered, so the chain breaks.
        global(kit.list, Stage::Copy, Access::None, Stage::ComputeShader, Access::ShaderWrite);
        kit.list.declareAccess(writes);
        kit.list.dispatch(1, 1, 1);
        kit.finish();
    });
}
