//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleKit.cpp
/// @brief Implements the helper that builds an upscaler and a valid set of upscale arguments on a recording list.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/UpscaleKit.h"

#include "Support/Backend.h"

#include <array>

namespace rojoRHI::test {
    //==================================================================================================================
    UpscalerDesc UpscaleKit::defaultDesc()
    {
        return UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "kit.upscaler" };
    }

    //==================================================================================================================
    UpscaleKit::UpscaleKit(QueueType type, const UpscalerDesc& description, bool validation, bool scopeTimings)
        : kit(type, validation, scopeTimings), desc(description), m_moveTextures(!validation && type == QueueType::Graphics && layoutsAreNative())
    {
        upscaler = *kit.device.createUpscaler(desc);
        // A new texture is Undefined (chapter 09). The cases that only record an upscale record no barrier, which Metal and the
        // Null backend ignore and a D3D12 upscale does not, since it names the layouts chapter 12 requires to the vendor call.
        params.color = moved(make(desc.color, desc.input, TextureUsage::Sampled), Layout::ShaderRead);
        params.depth = moved(make(desc.depth, desc.input, TextureUsage::Sampled | TextureUsage::DepthStencil), Layout::ShaderRead);
        params.motion = moved(make(desc.motion, desc.input, TextureUsage::Sampled), Layout::ShaderRead);
        params.output = moved(make(desc.outputFormat, desc.output, TextureUsage::Storage | TextureUsage::RenderTarget), Layout::ShaderWrite);
    }

    //==================================================================================================================
    Texture UpscaleKit::moved(Texture texture, Layout layout)
    {
        if (m_moveTextures)
        {
            transition(kit.list, texture, Layout::Undefined, layout);
        }
        return texture;
    }

    //==================================================================================================================
    Texture UpscaleKit::make(Format format, Extent2D extent, TextureUsage usage)
    {
        return *kit.device.createTexture(TextureDesc{ .format = format, .extent = { extent.width, extent.height, 1 }, .usage = usage, .label = "kit.upscale" });
    }

    //==================================================================================================================
    Texture UpscaleKit::exposureTexture()
    {
        return moved(make(Format::R16Float, { 1, 1 }, TextureUsage::Sampled), Layout::ShaderRead);
    }

    //==================================================================================================================
    Texture UpscaleKit::reactiveTexture()
    {
        return moved(make(Format::R8Unorm, desc.input, TextureUsage::Sampled), Layout::ShaderRead);
    }

    //==================================================================================================================
    Texture UpscaleKit::swapchainImage()
    {
        Swapchain swapchain = *kit.device.createSwapchain(SwapchainDesc{ .nativeSurface = surface(), .label = "kit.swapchain" });
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
