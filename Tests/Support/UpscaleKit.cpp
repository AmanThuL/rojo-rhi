//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleKit.cpp
/// @brief Implements the helper that builds an upscaler and a valid set of upscale arguments on a recording list.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/UpscaleKit.h"

#include <array>

namespace rojoRHI::test {
    namespace {
        /// Stand-in for a native surface: the Null backend never dereferences it.
        int g_surface = 0;
    } // namespace

    //==================================================================================================================
    UpscalerDesc UpscaleKit::defaultDesc()
    {
        return UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "kit.upscaler" };
    }

    //==================================================================================================================
    UpscaleKit::UpscaleKit(QueueType type, const UpscalerDesc& description) : kit(type), desc(description)
    {
        upscaler = *kit.device.createUpscaler(desc);
        params.color = make(desc.color, desc.input, TextureUsage::Sampled);
        params.depth = make(desc.depth, desc.input, TextureUsage::Sampled | TextureUsage::DepthStencil);
        params.motion = make(desc.motion, desc.input, TextureUsage::Sampled);
        params.output = make(desc.outputFormat, desc.output, TextureUsage::Storage | TextureUsage::RenderTarget);
    }

    //==================================================================================================================
    Texture UpscaleKit::make(Format format, Extent2D extent, TextureUsage usage)
    {
        return *kit.device.createTexture(TextureDesc{ .format = format, .extent = { extent.width, extent.height, 1 }, .usage = usage, .label = "kit.upscale" });
    }

    //==================================================================================================================
    Texture UpscaleKit::exposureTexture()
    {
        return make(Format::R16Float, { 1, 1 }, TextureUsage::Sampled);
    }

    //==================================================================================================================
    Texture UpscaleKit::reactiveTexture()
    {
        return make(Format::R8Unorm, desc.input, TextureUsage::Sampled);
    }

    //==================================================================================================================
    Texture UpscaleKit::swapchainImage()
    {
        Swapchain swapchain = *kit.device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface, .label = "kit.swapchain" });
        return swapchain.acquire();
    }

    //==================================================================================================================
    void UpscaleKit::upscale()
    {
        kit.list.upscale(upscaler, params);
    }

    //==================================================================================================================
    void UpscaleKit::nextFrame()
    {
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(kit.type).submit(lists);
        kit.device.endFrame();
        kit.device.beginFrame();
        kit.list = kit.device.acquireCommandList(kit.type, "kit.list");
    }
} // namespace rojoRHI::test
