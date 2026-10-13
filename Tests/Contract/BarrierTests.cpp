//----------------------------------------------------------------------------------------------------------------------
/// @file BarrierTests.cpp
/// @brief Tests barriers against chapter 09.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

//======================================================================================================================
TEST_CASE("an empty batch of barriers is accepted", "[contract]")
{
    PassKit kit;
    kit.list.barrier({});
    kit.finish();
}

//======================================================================================================================
TEST_CASE("one batch holds a global, a buffer and a texture barrier", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, 256);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage, 3, 2);
    const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::AllGraphics, Access::ShaderRead } };
    const std::array<BufferBarrier, 1> buffers{ BufferBarrier{ .buffer = buffer,
                                                               .offset = 64,
                                                               .size = 64,
                                                               .srcStage = Stage::ComputeShader,
                                                               .srcAccess = Access::ShaderWrite,
                                                               .dstStage = Stage::IndirectArgs,
                                                               .dstAccess = Access::IndirectArgs } };
    const std::array<TextureBarrier, 2> textures{ TextureBarrier{ .texture = texture,
                                                                  .range = { .baseMip = 0, .mipCount = 1 },
                                                                  .srcStage = Stage::None,
                                                                  .dstStage = Stage::ComputeShader,
                                                                  .dstAccess = Access::ShaderWrite,
                                                                  .newLayout = Layout::ShaderWrite },
                                                  TextureBarrier{ .texture = texture,
                                                                  .range = { .baseMip = 1, .mipCount = kAllMips },
                                                                  .srcStage = Stage::All,
                                                                  .dstStage = Stage::FragmentShader,
                                                                  .dstAccess = Access::ShaderRead,
                                                                  .oldLayout = Layout::Undefined,
                                                                  .newLayout = Layout::ShaderRead } };
    kit.list.barrier(globals, buffers, textures);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a buffer barrier names the whole buffer, its last byte or a range ending at its end", "[contract]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::None, 256);
    const std::array<BufferBarrier, 4> buffers{
        BufferBarrier{ .buffer = buffer, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::All, .dstAccess = Access::ShaderRead },
        BufferBarrier{ .buffer = buffer, .offset = 255, .size = 1, .srcStage = Stage::Copy, .dstStage = Stage::All },
        BufferBarrier{ .buffer = buffer, .offset = 128, .size = 128, .srcStage = Stage::Copy, .dstStage = Stage::All },
        BufferBarrier{ .buffer = buffer, .offset = 255, .size = kWholeSize, .srcStage = Stage::Copy, .dstStage = Stage::All },
    };
    kit.list.barrier({}, buffers);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a texture barrier discards from Undefined and moves a texture through the layouts its usage allows", "[contract]")
{
    PassKit kit;
    const Texture color = kit.texture(Format::RGBA8Unorm, TextureUsage::RenderTarget | TextureUsage::Sampled);
    const Texture depth = kit.depthTarget();
    const std::array<TextureBarrier, 2> first{
        TextureBarrier{ .texture = color,
                        .dstStage = Stage::ColorAttachment,
                        .dstAccess = Access::ColorAttachmentWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::ColorAttachment },
        TextureBarrier{ .texture = depth,
                        .dstStage = Stage::DepthStencil,
                        .dstAccess = Access::DepthStencilWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::DepthStencilWrite },
    };
    kit.list.barrier({}, {}, first);
    const std::array<TextureBarrier, 1> second{ TextureBarrier{ .texture = color,
                                                                .srcStage = Stage::ColorAttachment,
                                                                .srcAccess = Access::ColorAttachmentWrite,
                                                                .dstStage = Stage::FragmentShader,
                                                                .dstAccess = Access::ShaderRead,
                                                                .oldLayout = Layout::ColorAttachment,
                                                                .newLayout = Layout::ShaderRead } };
    kit.list.barrier({}, {}, second);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("texture barriers of one batch on disjoint subresources or on different textures are accepted", "[contract]")
{
    PassKit kit;
    const Texture a = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 4, 2);
    const Texture b = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 4, 2);
    const std::array<TextureBarrier, 4> textures{
        TextureBarrier{ .texture = a, .range = { .baseMip = 0, .mipCount = 2, .baseLayer = 0, .layerCount = 1 }, .newLayout = Layout::General },
        TextureBarrier{ .texture = a, .range = { .baseMip = 2, .mipCount = kAllMips, .baseLayer = 0, .layerCount = 1 }, .newLayout = Layout::General },
        TextureBarrier{ .texture = a, .range = { .baseMip = 0, .mipCount = kAllMips, .baseLayer = 1, .layerCount = kAllLayers }, .newLayout = Layout::General },
        TextureBarrier{ .texture = b, .newLayout = Layout::General },
    };
    kit.list.barrier({}, {}, textures);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a Compute list takes barriers over its stages and layouts", "[contract]")
{
    PassKit kit(QueueType::Compute);
    const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage);
    const std::array<GlobalBarrier, 2> globals{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderRead },
                                                GlobalBarrier{ Stage::All, Access::None, Stage::IndirectArgs, Access::IndirectArgs } };
    const std::array<TextureBarrier, 1> textures{ TextureBarrier{ .texture = texture,
                                                                  .srcStage = Stage::Copy,
                                                                  .srcAccess = Access::CopyWrite,
                                                                  .dstStage = Stage::ComputeShader,
                                                                  .dstAccess = Access::ShaderWrite,
                                                                  .oldLayout = Layout::General,
                                                                  .newLayout = Layout::ShaderWrite } };
    kit.list.barrier(globals, {}, textures);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a Copy list takes barriers over the Copy stage", "[contract]")
{
    PassKit kit(QueueType::Copy);
    const std::array<GlobalBarrier, 2> globals{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyRead },
                                                GlobalBarrier{ Stage::All, Access::None, Stage::Copy, Access::None } };
    kit.list.barrier(globals);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("barriers are recorded again after a render pass ends", "[contract]")
{
    PassKit kit;
    kit.beginSimplePass();
    kit.list.endRenderPass();
    const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::Copy, Access::CopyRead } };
    kit.list.barrier(globals);
    kit.finish();
}
