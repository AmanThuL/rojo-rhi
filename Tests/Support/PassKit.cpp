//----------------------------------------------------------------------------------------------------------------------
/// @file PassKit.cpp
/// @brief Implements the helper that builds a device, a recording list, attachment textures and pipelines for the render pass tests.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/PassKit.h"

#include "Support/NullLibrary.h"

#include <array>
#include <vector>

namespace rojoRHI::test {
    //==================================================================================================================
    PassKit::PassKit(QueueType listType) : type(listType)
    {
        device = *createDevice();
        const std::vector<std::byte> blob = standardLibrary();
        library = *device.loadShaderLibrary(blob, "kit.library");
        device.beginFrame();
        list = device.acquireCommandList(type, "kit.list");
    }

    //==================================================================================================================
    Texture PassKit::target(Format format, uint32_t samples, uint32_t mips, uint32_t layers)
    {
        return *device.createTexture(
            TextureDesc{ .type = layers > 1 ? TextureType::Tex2DArray : TextureType::Tex2D,
                         .format = format,
                         .extent = { kSize, kSize, 1 },
                         .mips = mips,
                         .layers = layers,
                         .samples = samples,
                         .usage = TextureUsage::RenderTarget,
                         .label = "kit.target" }
        );
    }

    //==================================================================================================================
    Texture PassKit::depthTarget(Format format, uint32_t samples)
    {
        return *device.createTexture(
            TextureDesc{ .format = format, .extent = { kSize, kSize, 1 }, .samples = samples, .usage = TextureUsage::DepthStencil, .label = "kit.depth" }
        );
    }

    //==================================================================================================================
    Pipeline PassKit::graphics(std::span<const Format> colors, Format depth, uint32_t samples, PrimitiveClass primitive, bool depthBias, bool mesh)
    {
        std::vector<ColorTarget> targets;
        for (const Format format : colors)
        {
            targets.push_back(ColorTarget{ .format = format });
        }
        GraphicsPipelineDesc desc{ .primitive = primitive, .colors = targets, .depthFormat = depth, .samples = samples, .label = "kit.pipeline" };
        if (mesh)
        {
            desc.mesh = { library, "msMain" };
        }
        else
        {
            desc.vertex = { library, "vsMain" };
        }
        desc.raster.depthBias = depthBias;
        return *device.createPipeline(desc);
    }

    //==================================================================================================================
    Pipeline PassKit::compute()
    {
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "kit.compute" });
    }

    //==================================================================================================================
    void PassKit::beginSimplePass()
    {
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = target() } };
        list.beginRenderPass(fullPass(colors));
    }

    //==================================================================================================================
    Buffer PassKit::buffer(BufferUsage usage, uint64_t size, Memory memory)
    {
        return *device.createBuffer(BufferDesc{ .size = size, .memory = memory, .usage = usage, .label = "kit.buffer" });
    }

    //==================================================================================================================
    Texture PassKit::texture(Format format, TextureUsage usage, uint32_t mips, uint32_t layers)
    {
        return *device.createTexture(
            TextureDesc{ .type = layers > 1 ? TextureType::Tex2DArray : TextureType::Tex2D,
                         .format = format,
                         .extent = { kSize, kSize, 1 },
                         .mips = mips,
                         .layers = layers,
                         .usage = usage,
                         .label = "kit.texture" }
        );
    }

    //==================================================================================================================
    void PassKit::readyToDraw(bool mesh)
    {
        constexpr std::array<Format, 1> colors = { Format::RGBA8Unorm };
        beginSimplePass();
        list.setPipeline(graphics(colors, Format::Undefined, 1, PrimitiveClass::Triangle, false, mesh));
        list.setViewport({ .width = static_cast<float>(kSize), .height = static_cast<float>(kSize) });
        list.setScissor({ 0, 0, kSize, kSize });
    }

    //==================================================================================================================
    void PassKit::finish()
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
        device.endFrame();
        destroyDevice(device);
    }

    //==================================================================================================================
    RenderPassDesc fullPass(std::span<const ColorAttachment> colors)
    {
        return RenderPassDesc{ .colors = colors, .area = { 0, 0, PassKit::kSize, PassKit::kSize }, .label = "kit.pass" };
    }
} // namespace rojoRHI::test
