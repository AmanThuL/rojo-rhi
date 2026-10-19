//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleDeathTests.cpp
/// @brief Death tests for the valid-usage rules of upscaling in chapter 12, and the swapchain-image rule at each texture it takes.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace rojoRHI;
using rojoRHI::test::UpscaleKit;

namespace {
    //==================================================================================================================
    /// Creates an upscaler on a device with `desc` changed by `change`.
    template<typename Change> void createWith(Change change)
    {
        Device device = *createDevice();
        UpscalerDesc desc = UpscaleKit::defaultDesc();
        change(desc);
        (void)device.createUpscaler(desc);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0001 the input width is zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0001", [] { createWith([](UpscalerDesc& desc) { desc.input.width = 0; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0001 the input height is zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0001", [] { createWith([](UpscalerDesc& desc) { desc.input.height = 0; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0002 the output is narrower than the input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0002", [] { createWith([](UpscalerDesc& desc) { desc.output.width = desc.input.width - 1; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0002 the output is shorter than the input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0002", [] { createWith([](UpscalerDesc& desc) { desc.output.height = desc.input.height - 1; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0003 the color format is a depth format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0003", [] { createWith([](UpscalerDesc& desc) { desc.color = Format::D32Float; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0003 the output format is a block-compressed format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0003", [] { createWith([](UpscalerDesc& desc) { desc.outputFormat = Format::BC1Unorm; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0003 the depth format is a color format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0003", [] { createWith([](UpscalerDesc& desc) { desc.depth = Format::RGBA16Float; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0003 the motion format has four channels", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0003", [] { createWith([](UpscalerDesc& desc) { desc.motion = Format::RGBA16Float; }); });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0004 preExposure is zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0004", [] {
        UpscaleKit kit;
        kit.params.preExposure = 0.0f;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0004 preExposure is negative", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0004", [] {
        UpscaleKit kit;
        kit.params.preExposure = -1.0f;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0005 sharpness is below zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0005", [] {
        UpscaleKit kit;
        kit.params.sharpness = -0.5f;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0005 sharpness is above one", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0005", [] {
        UpscaleKit kit;
        kit.params.sharpness = 1.5f;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0006 the list is a Compute list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0006", [] {
        UpscaleKit kit{ QueueType::Compute };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0006 the list is a Copy list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0006", [] {
        UpscaleKit kit{ QueueType::Copy };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0007 a render pass is open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0007", [] {
        UpscaleKit kit;
        kit.kit.beginSimplePass();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0008 one upscaler upscales twice in a frame", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0008", [] {
        UpscaleKit kit;
        kit.upscale();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0009 the color texture is null", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0009", [] {
        UpscaleKit kit;
        kit.params.color = {};
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0009 the depth texture is null", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0009", [] {
        UpscaleKit kit;
        kit.params.depth = {};
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0009 the motion texture is null", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0009", [] {
        UpscaleKit kit;
        kit.params.motion = {};
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0009 the output texture is null", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0009", [] {
        UpscaleKit kit;
        kit.params.output = {};
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the color format differs from the descriptor's", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.color = kit.make(Format::RGBA8Unorm, kit.desc.input, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the depth format differs from the descriptor's", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.depth = kit.make(Format::D32FloatS8, kit.desc.input, TextureUsage::Sampled | TextureUsage::DepthStencil);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the motion format differs from the descriptor's", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.motion = kit.make(Format::RGBA16Float, kit.desc.input, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the color extent is not the input extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.color = kit.make(kit.desc.color, { kit.desc.input.width / 2, kit.desc.input.height }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the depth extent is not the input extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.depth = kit.make(kit.desc.depth, { kit.desc.input.width, kit.desc.input.height * 2 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the motion extent is not the input extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.motion = kit.make(kit.desc.motion, kit.desc.output, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output format differs from the descriptor's", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscaleKit kit;
        kit.params.output = kit.make(Format::RGBA8Unorm, kit.desc.output, TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output extent is not the descriptor's output extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscaleKit kit;
        kit.params.output = kit.make(kit.desc.outputFormat, kit.desc.input, TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output is also the color input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscalerDesc desc = UpscaleKit::defaultDesc();
        desc.output = desc.input;
        UpscaleKit kit{ QueueType::Graphics, desc };
        kit.params.color = kit.make(desc.color, desc.input, TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.params.output = kit.params.color;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output is also the reactive input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscalerDesc desc = UpscaleKit::defaultDesc();
        desc.output = desc.input;
        desc.outputFormat = Format::R8Unorm;
        UpscaleKit kit{ QueueType::Graphics, desc };
        kit.params.output = kit.make(Format::R8Unorm, desc.input, TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.params.reactive = kit.params.output;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the exposure texture has another format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.exposure = kit.make(Format::R32Float, { 1, 1 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the exposure texture is larger than one texel", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.exposure = kit.make(Format::R16Float, { 2, 1 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the reactive texture has another format", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.make(Format::R16Float, kit.desc.input, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the reactive texture is not the input extent", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.make(Format::R8Unorm, kit.desc.output, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0013 the color input is not sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0013", [] {
        UpscaleKit kit;
        kit.params.color = kit.make(kit.desc.color, kit.desc.input, TextureUsage::Storage);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0013 the depth input is not sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0013", [] {
        UpscaleKit kit;
        kit.params.depth = kit.make(kit.desc.depth, kit.desc.input, TextureUsage::DepthStencil);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0013 the motion input is not sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0013", [] {
        UpscaleKit kit;
        kit.params.motion = kit.make(kit.desc.motion, kit.desc.input, TextureUsage::Storage);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0013 the exposure input is not sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0013", [] {
        UpscaleKit kit;
        kit.params.exposure = kit.make(Format::R16Float, { 1, 1 }, TextureUsage::Storage);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0013 the reactive input is not sampled", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0013", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.make(Format::R8Unorm, kit.desc.input, TextureUsage::Storage);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0014 the output has no storage usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0014", [] {
        UpscaleKit kit;
        kit.params.output = kit.make(kit.desc.outputFormat, kit.desc.output, TextureUsage::RenderTarget);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0014 the output has no render target usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0014", [] {
        UpscaleKit kit;
        kit.params.output = kit.make(kit.desc.outputFormat, kit.desc.output, TextureUsage::Storage);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0015 the content width is zero and its height is not", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0015", [] {
        UpscaleKit kit;
        kit.params.inputContent = { 0, 8 };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0015 the content height is zero and its width is not", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0015", [] {
        UpscaleKit kit;
        kit.params.inputContent = { 8, 0 };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0015 the content is wider than the input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0015", [] {
        UpscaleKit kit;
        kit.params.inputContent = { kit.desc.input.width + 1, 8 };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0015 the content is taller than the input", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0015", [] {
        UpscaleKit kit;
        kit.params.inputContent = { 8, kit.desc.input.height + 1 };
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale color", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.color = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale depth", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.depth = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale motion", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.motion = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale output", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.output = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale exposure", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.exposure = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-present-0012 a swapchain image is the upscale reactive", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-present-0012", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.swapchainImage();
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0008 two lists of one frame each upscale the same upscaler", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0008", [] {
        UpscaleKit kit;
        const CommandList second = kit.kit.device.acquireCommandList(QueueType::Graphics, "kit.second");
        kit.upscale();
        CommandList list = second;
        list.upscale(kit.upscaler, kit.params);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output is also the exposure input", "[contract][death]")
{
    UpscalerDesc desc = UpscaleKit::defaultDesc();
    desc.input = { 1, 1 };
    desc.output = { 1, 1 };
    desc.outputFormat = Format::R16Float;
    // The 1x1 extent lets the output stand in as the 1x1 exposure; creation may refuse it ("when the implementation rejects the extent
    // ratio").
    Device device = *createDevice();
    const Result<Upscaler> created = device.createUpscaler(desc);
    if (!created)
    {
        const std::string message = created.error().message;
        destroyDevice(device);
        SKIP("the device refuses a 1x1 upscaler: " << message);
    }
    device.destroy(*created);
    destroyDevice(device);
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [desc] {
        UpscaleKit kit{ QueueType::Graphics, desc };
        kit.params.output = kit.make(Format::R16Float, desc.input, TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.params.exposure = kit.params.output;
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the color height is not the input height", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.color = kit.make(kit.desc.color, { kit.desc.input.width, kit.desc.input.height / 2 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the depth width is not the input width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.depth = kit.make(kit.desc.depth, { kit.desc.input.width * 2, kit.desc.input.height }, TextureUsage::Sampled | TextureUsage::DepthStencil);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the motion width is not the input width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.motion = kit.make(kit.desc.motion, { kit.desc.input.width * 2, kit.desc.input.height }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0010 the motion height is not the input height", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0010", [] {
        UpscaleKit kit;
        kit.params.motion = kit.make(kit.desc.motion, { kit.desc.input.width, kit.desc.input.height * 2 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output width is not the descriptor's output width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscaleKit kit;
        kit.params.output =
            kit.make(kit.desc.outputFormat, { kit.desc.input.width, kit.desc.output.height }, TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0011 the output height is not the descriptor's output height", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0011", [] {
        UpscaleKit kit;
        kit.params.output =
            kit.make(kit.desc.outputFormat, { kit.desc.output.width, kit.desc.input.height }, TextureUsage::Storage | TextureUsage::RenderTarget);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the exposure texture is taller than one texel", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.exposure = kit.make(Format::R16Float, { 1, 2 }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the reactive width is not the input width", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.make(Format::R8Unorm, { kit.desc.input.width * 2, kit.desc.input.height }, TextureUsage::Sampled);
        kit.upscale();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-upscale-0012 the reactive height is not the input height", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-upscale-0012", [] {
        UpscaleKit kit;
        kit.params.reactive = kit.make(Format::R8Unorm, { kit.desc.input.width, kit.desc.input.height * 2 }, TextureUsage::Sampled);
        kit.upscale();
    });
}
