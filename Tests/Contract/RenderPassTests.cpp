//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPassTests.cpp
/// @brief Tests render passes, scopes, pipeline binding and dynamic state against chapters 06 and 07.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    constexpr std::array<Format, 1> kRgba8 = { Format::RGBA8Unorm };
} // namespace

//======================================================================================================================
TEST_CASE("a render pass over one color attachment begins and ends", "[contract]")
{
    PassKit kit;
    kit.beginSimplePass();
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a render pass over depth alone begins and ends", "[contract]")
{
    PassKit kit;
    kit.list.beginRenderPass(RenderPassDesc{ .depth = { .texture = kit.depthTarget(), .clearDepth = 1.0f }, .area = { 0, 0, PassKit::kSize, PassKit::kSize } });
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a render pass takes the maximum number of color attachments of distinct textures", "[contract]")
{
    PassKit kit;
    std::array<ColorAttachment, 8> colors;
    for (ColorAttachment& color : colors)
    {
        color.texture = kit.target(Format::R8Unorm);
    }
    kit.list.beginRenderPass(test::fullPass(colors));
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a render pass takes a mip and a layer of an attachment texture", "[contract]")
{
    PassKit kit;
    const Texture texture = kit.target(Format::RGBA8Unorm, 1, 3, 4);
    const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = texture, .mip = 1, .layer = 2 },
                                                 ColorAttachment{ .texture = texture, .mip = 1, .layer = 3 } };
    kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, PassKit::kSize / 2, PassKit::kSize / 2 } });
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a multisampled attachment resolves into a single-sample texture", "[contract]")
{
    PassKit kit;
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{
        .texture = kit.target(Format::RGBA8Unorm, 4), .store = StoreOp::DontCare, .resolve = kit.target() } };
    kit.list.beginRenderPass(test::fullPass(colors));
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a pass takes color and depth attachments with every load and store operation", "[contract]")
{
    PassKit kit;
    for (const LoadOp load : { LoadOp::Load, LoadOp::Clear, LoadOp::DontCare })
    {
        for (const StoreOp store : { StoreOp::Store, StoreOp::DontCare })
        {
            const std::array<ColorAttachment, 1> colors{ ColorAttachment{
                .texture = kit.target(), .load = load, .store = store, .clear = { 1.0f, 0.0f, 0.0f, 1.0f } } };
            RenderPassDesc desc = test::fullPass(colors);
            desc.depth = {
                .texture = kit.depthTarget(Format::D32FloatS8), .load = load, .store = store, .stencilLoad = load, .stencilStore = store, .clearStencil = 3
            };
            kit.list.beginRenderPass(desc);
            kit.list.endRenderPass();
        }
    }
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a pass renders to a sub-area of its attachments", "[contract]")
{
    PassKit kit;
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
    kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 8, 16, 32, 32 } });
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("scopes nest outside and inside a render pass", "[contract]")
{
    PassKit kit;
    kit.list.beginScope("outer");
    kit.list.beginScope("inner");
    kit.list.endScope();
    kit.beginSimplePass();
    kit.list.beginScope("in pass");
    kit.list.beginScope("nested in pass");
    kit.list.endScope();
    kit.list.endScope();
    kit.list.endRenderPass();
    kit.list.endScope();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("scopes are valid on every queue type", "[contract]")
{
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        PassKit kit(type);
        kit.list.beginScope("scope");
        kit.list.endScope();
        kit.finish();
    }
}

//======================================================================================================================
TEST_CASE("a graphics pipeline binds inside a pass that matches it", "[contract]")
{
    PassKit kit;
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(Format::RGBA16Float, 4) } };
    constexpr std::array<Format, 1> formats = { Format::RGBA16Float };
    RenderPassDesc desc = test::fullPass(colors);
    desc.depth = { .texture = kit.depthTarget(Format::D32Float, 4) };
    kit.list.beginRenderPass(desc);
    kit.list.setPipeline(kit.graphics(formats, Format::D32Float, 4));
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a depth-only pass binds a depth-only pipeline", "[contract]")
{
    PassKit kit;
    kit.list.beginRenderPass(RenderPassDesc{ .depth = { .texture = kit.depthTarget() }, .area = { 0, 0, PassKit::kSize, PassKit::kSize } });
    kit.list.setPipeline(kit.graphics({}, Format::D32Float));
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a pass binds a pipeline again and a mesh pipeline", "[contract]")
{
    PassKit kit;
    kit.beginSimplePass();
    kit.list.setPipeline(kit.graphics(kRgba8));
    kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Line, false, true));
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a compute pipeline binds outside a pass on a Graphics or Compute list and again after a pass", "[contract]")
{
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute })
    {
        PassKit kit(type);
        const Pipeline compute = kit.compute();
        kit.list.setPipeline(compute);
        if (type == QueueType::Graphics)
        {
            kit.beginSimplePass();
            kit.list.endRenderPass();
            kit.list.setPipeline(compute);
        }
        kit.finish();
    }
}

//======================================================================================================================
TEST_CASE("dynamic state is set inside a pass", "[contract]")
{
    PassKit kit;
    kit.beginSimplePass();
    kit.list.setViewport({ .x = 4.0f, .y = 4.0f, .width = 32.0f, .height = 32.0f, .minDepth = 0.25f, .maxDepth = 0.75f });
    kit.list.setViewport({ .width = 1.0f, .height = 1.0f });
    kit.list.setScissor({ 0, 0, PassKit::kSize, PassKit::kSize });
    kit.list.setScissor({ 63, 63, 1, 1 });
    kit.list.setStencilReference(255);
    kit.list.setBlendConstants(0.5f, 0.25f, 0.0f, 1.0f);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a scissor lies within the area of a pass that does not start at the origin", "[contract]")
{
    PassKit kit;
    const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
    kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 8, 8, 16, 16 } });
    kit.list.setScissor({ 8, 8, 16, 16 });
    kit.list.setScissor({ 12, 10, 4, 4 });
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("every topology of the pipeline's class is set after a pipeline binds", "[contract]")
{
    struct Case
    {
        PrimitiveClass primitive;
        std::array<PrimitiveTopology, 2> topologies;
    };
    const std::array<Case, 3> cases{
        Case{ PrimitiveClass::Point, { PrimitiveTopology::PointList, PrimitiveTopology::PointList } },
        Case{ PrimitiveClass::Line, { PrimitiveTopology::LineList, PrimitiveTopology::LineStrip } },
        Case{ PrimitiveClass::Triangle, { PrimitiveTopology::TriangleList, PrimitiveTopology::TriangleStrip } },
    };
    PassKit kit;
    kit.beginSimplePass();
    for (const Case& item : cases)
    {
        kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, item.primitive));
        for (const PrimitiveTopology topology : item.topologies)
        {
            kit.list.setPrimitiveTopology(topology);
        }
    }
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a depth bias is set while the bound pipeline has depth bias", "[contract]")
{
    PassKit kit;
    kit.beginSimplePass();
    kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Triangle, true));
    kit.list.setDepthBias(1.0f, 2.0f, 0.0f);
    kit.list.setDepthBias(0.0f, 0.0f, 0.5f);
    kit.list.endRenderPass();
    kit.finish();
}

//======================================================================================================================
TEST_CASE("a list records a second pass after the first ends", "[contract]")
{
    PassKit kit;
    for (int pass = 0; pass < 3; ++pass)
    {
        kit.beginSimplePass();
        kit.list.setViewport({ .width = 8.0f, .height = 8.0f });
        kit.list.endRenderPass();
    }
    kit.finish();
}
