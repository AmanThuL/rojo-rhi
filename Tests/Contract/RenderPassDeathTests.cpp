//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPassDeathTests.cpp
/// @brief Death tests for the valid-usage rules of render passes, scopes, pipeline binding and dynamic state.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

namespace {
    constexpr std::array<Format, 1> kRgba8 = { Format::RGBA8Unorm };
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0003 a list ends with a render pass open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0003", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.end();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0004 a list ends with a scope open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0004", [] {
        PassKit kit;
        kit.list.beginScope("scope");
        kit.list.end();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0063 a scope begins on a list that has ended", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0063", [] {
        PassKit kit;
        kit.list.end();
        kit.list.beginScope("scope");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0063 a command is recorded on a list that was submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0063", [] {
        PassKit kit;
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(QueueType::Graphics).submit(lists);
        kit.list.setViewport({ .width = 1.0f, .height = 1.0f });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0005 a color attachment is not a RenderTarget texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0005", [] {
        PassKit kit;
        const Texture sampled = *kit.device.createTexture(TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::Sampled });
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = sampled } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0005 a color attachment names a layer the texture lacks", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0005", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(), .layer = 1 } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0071 a resolve target is given for a single-sample attachment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0071", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(), .resolve = kit.target() } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0071 a resolve target has another format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0071", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 4),
                                                                      .resolve = kit.target(Format::BGRA8Unorm) } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0003 a resolve target's format cannot resolve", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0003", [] {
        PassKit kit;
        // The Null backend resolves every color format, so the test withdraws the capability through the core.
        FormatCapabilities capabilities = kit.device.formatCapabilities(Format::RGBA8Unorm);
        capabilities.resolve = false;
        core::coreOf(kit.device).setFormatCapabilities(Format::RGBA8Unorm, capabilities);
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 4), .resolve = kit.target() } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0008 a depth attachment is not a DepthStencil texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0008", [] {
        PassKit kit;
        kit.list.beginRenderPass(RenderPassDesc{ .depth = { .texture = kit.target() }, .area = { 0, 0, PassKit::kSize, PassKit::kSize } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0009 the depth clear value is above one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0009", [] {
        PassKit kit;
        kit.list.beginRenderPass(
            RenderPassDesc{ .depth = { .texture = kit.depthTarget(), .clearDepth = 2.0f }, .area = { 0, 0, PassKit::kSize, PassKit::kSize } }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0011 a pass has more color attachments than the limit", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0011", [] {
        PassKit kit;
        const std::array<ColorAttachment, 9> colors = [&] {
            std::array<ColorAttachment, 9> all;
            all.fill(ColorAttachment{ .texture = kit.target() });
            return all;
        }();
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0011 a pass has no attachment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0011", [] {
        PassKit kit;
        kit.list.beginRenderPass(RenderPassDesc{ .area = { 0, 0, 1, 1 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0012 attachments differ in extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0012", [] {
        PassKit kit;
        const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = kit.target() },
                                                     ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 1, 2), .mip = 1 } };
        kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 16, 16 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0012 attachments differ in sample count", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0012", [] {
        PassKit kit;
        const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = kit.target() },
                                                     ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm, 4) } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0013 the pass area is empty", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0013", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 0, 8 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0013 the pass area exceeds the attachments", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0013", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 32, 0, 33, 8 } });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0014 a subresource appears twice among the attachments", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0014", [] {
        PassKit kit;
        const Texture texture = kit.target();
        const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = texture }, ColorAttachment{ .texture = texture } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0015 the color formats take more bytes per sample than the limit", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0015", [] {
        PassKit kit;
        // A back door: the test lowers the Null limit of 64 bytes through the core, which no caller can do, so two RGBA8
        // targets of 8 bytes per sample exceed it.
        core::coreOf(kit.device).capabilities().limits.maxColorBytesPerSample = 7;
        const std::array<ColorAttachment, 2> colors{ ColorAttachment{ .texture = kit.target() }, ColorAttachment{ .texture = kit.target(Format::RGBA8Unorm) } };
        kit.list.beginRenderPass(test::fullPass(colors));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0016 a render pass begins on a Compute list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0016", [] {
        PassKit kit(QueueType::Compute);
        kit.beginSimplePass();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0016 a render pass begins inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0016", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.beginSimplePass();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0018 a render pass ends with none open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0018", [] {
        PassKit kit;
        kit.list.endRenderPass();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0019 a render pass ends with a scope begun inside it open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0019", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.beginScope("scope");
        kit.list.endRenderPass();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0020 the null pipeline is bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0020", [] {
        PassKit kit;
        kit.list.setPipeline(Pipeline{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0020 a destroyed pipeline is bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0020", [] {
        PassKit kit;
        const Pipeline pipeline = kit.compute();
        kit.device.destroy(pipeline);
        kit.list.setPipeline(pipeline);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0021 a graphics pipeline is bound outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0021", [] {
        PassKit kit;
        kit.list.setPipeline(kit.graphics(kRgba8));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0021 a graphics pipeline with other color formats is bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0021", [] {
        PassKit kit;
        kit.beginSimplePass();
        constexpr std::array<Format, 1> other = { Format::RGBA16Float };
        kit.list.setPipeline(kit.graphics(other));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0021 a graphics pipeline with another depth format is bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0021", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8, Format::D32Float));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0021 a graphics pipeline with another sample count is bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0021", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 4));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0022 a compute pipeline is bound inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0022", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.compute());
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0022 a compute pipeline is bound on a Copy list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0022", [] {
        PassKit kit(QueueType::Copy);
        kit.list.setPipeline(kit.compute());
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0032 a scope ends with none open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0032", [] {
        PassKit kit;
        kit.list.endScope();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0033 a scope begun outside a render pass ends inside one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0033", [] {
        PassKit kit;
        kit.list.beginScope("scope");
        kit.beginSimplePass();
        kit.list.endScope();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0015 a viewport is set outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0015", [] {
        PassKit kit;
        kit.list.setViewport({ .width = 8.0f, .height = 8.0f });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0016 a viewport has no width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0016", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setViewport({ .width = 0.0f, .height = 8.0f });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0017 a viewport depth range leaves zero to one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0017", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setViewport({ .width = 8.0f, .height = 8.0f, .maxDepth = 1.5f });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0018 a scissor is set outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0018", [] {
        PassKit kit;
        kit.list.setScissor({ 0, 0, 8, 8 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0019 a scissor leaves the pass area", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0019", [] {
        PassKit kit;
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = kit.target() } };
        kit.list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 8, 8, 16, 16 } });
        kit.list.setScissor({ 0, 8, 16, 16 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0020 a stencil reference is set outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0020", [] {
        PassKit kit;
        kit.list.setStencilReference(1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0021 blend constants are set outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0021", [] {
        PassKit kit;
        kit.list.setBlendConstants(0.0f, 0.0f, 0.0f, 0.0f);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0022 a topology is set with no pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0022", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPrimitiveTopology(PrimitiveTopology::TriangleList);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0022 a topology is set with a mesh pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0022", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Triangle, false, true));
        kit.list.setPrimitiveTopology(PrimitiveTopology::TriangleList);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0023 a topology of another class is set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0023", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.setPrimitiveTopology(PrimitiveTopology::LineStrip);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0024 a depth bias is set outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0024", [] {
        PassKit kit;
        kit.list.setDepthBias(1.0f, 1.0f, 0.0f);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-pipeline-0025 a depth bias is set with a pipeline created without it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-pipeline-0025", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.setDepthBias(1.0f, 1.0f, 0.0f);
    });
}
