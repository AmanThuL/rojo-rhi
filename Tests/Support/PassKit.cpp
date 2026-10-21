//----------------------------------------------------------------------------------------------------------------------
/// @file PassKit.cpp
/// @brief Implements the helper that builds a device, a recording list, attachment textures and pipelines for the render pass tests.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/PassKit.h"

#include "Support/Backend.h"

#include <array>
#include <vector>

namespace rojoRHI::test {
    //==================================================================================================================
    PassKit::PassKit(QueueType listType, bool validation, bool scopeTimings)
        : type(listType), m_validation(validation), m_moveTargets(!validation && layoutsAreNative())
    {
        device = *createDevice(DeviceDesc{ .validation = validation, .scopeTimings = scopeTimings });
        device.beginFrame();
        list = device.acquireCommandList(type, "kit.list");
    }

    //==================================================================================================================
    ShaderLibrary PassKit::ensureLibrary()
    {
        if (library.bits == 0)
        {
            const std::vector<std::byte> blob = standardLibrary();
            library = *device.loadShaderLibrary(blob, "kit.library");
        }
        return library;
    }

    //==================================================================================================================
    Texture PassKit::target(Format format, uint32_t samples, uint32_t mips, uint32_t layers)
    {
        const Texture texture = *device.createTexture(
            TextureDesc{ .type = layers > 1 ? TextureType::Tex2DArray : TextureType::Tex2D,
                         .format = format,
                         .extent = { kSize, kSize, 1 },
                         .mips = mips,
                         .layers = layers,
                         .samples = samples,
                         .usage = TextureUsage::RenderTarget,
                         .label = "kit.target" }
        );
        if (m_moveTargets && type == QueueType::Graphics)
        {
            // A new texture is Undefined (chapter 09). The cases that only begin and end passes record no barrier, which
            // Metal and the Null backend ignore and the D3D12 runtime rejects, so the kit moves the texture for them.
            transition(list, texture, Layout::Undefined, Layout::ColorAttachment);
        }
        return texture;
    }

    //==================================================================================================================
    Texture PassKit::depthTarget(Format format, uint32_t samples)
    {
        const Texture texture = *device.createTexture(
            TextureDesc{ .format = format, .extent = { kSize, kSize, 1 }, .samples = samples, .usage = TextureUsage::DepthStencil, .label = "kit.depth" }
        );
        if (m_moveTargets && type == QueueType::Graphics)
        {
            transition(list, texture, Layout::Undefined, Layout::DepthStencilWrite);
        }
        return texture;
    }

    //==================================================================================================================
    Pipeline PassKit::graphics(std::span<const Format> colors, Format depth, uint32_t samples, PrimitiveClass primitive, bool depthBias, bool mesh)
    {
        const ShaderLibrary loaded = ensureLibrary();
        std::vector<ColorTarget> targets;
        for (const Format format : colors)
        {
            targets.push_back(ColorTarget{ .format = format });
        }
        GraphicsPipelineDesc desc{ .primitive = primitive, .colors = targets, .depthFormat = depth, .samples = samples, .label = "kit.pipeline" };
        if (mesh)
        {
            desc.mesh = { loaded, "msMain" };
        }
        else
        {
            desc.vertex = { loaded, "vsMain" };
        }
        desc.raster.depthBias = depthBias;
        return *device.createPipeline(desc);
    }

    //==================================================================================================================
    Pipeline PassKit::graphicsWith(std::span<const ColorTarget> targets, Format depth, const DepthStencilState& depthStencil, uint32_t samples)
    {
        const ShaderLibrary loaded = ensureLibrary();
        GraphicsPipelineDesc desc{
            .vertex = { loaded, "vsMain" }, .depthStencil = depthStencil, .colors = targets, .depthFormat = depth, .samples = samples, .label = "kit.pipeline"
        };
        return *device.createPipeline(desc);
    }

    //==================================================================================================================
    Pipeline PassKit::compute()
    {
        const ShaderLibrary loaded = ensureLibrary();
        return *device.createPipeline(ComputePipelineDesc{ .compute = { loaded, "csMain" }, .label = "kit.compute" });
    }

    //==================================================================================================================
    void PassKit::beginSimplePass()
    {
        if (m_moveTargets && m_simpleTarget.bits == 0)
        {
            m_simpleTarget = target();
        }
        const Texture color = m_moveTargets ? m_simpleTarget : target();
        if (m_validation)
        {
            // The tracker judges the attachment's layout and the barrier before it at submission, so a validating kit moves it
            // first.
            transition(list, color, Layout::Undefined, Layout::ColorAttachment);
        }
        const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = color } };
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
    void PassKit::joinQueues()
    {
        if (m_join.bits == 0)
        {
            m_join = *device.createFence(0, "kit.join");
        }
        for (const QueueType other : { QueueType::Compute, QueueType::Copy })
        {
            const std::array<FenceValue, 1> joined{ FenceValue{ m_join, ++m_joined } };
            device.queue(other).submit({}, {}, joined);
            device.queue(QueueType::Graphics).submit({}, joined);
        }
    }

    //==================================================================================================================
    void PassKit::finish()
    {
        list.end();
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
        if (m_validation)
        {
            joinQueues();
        }
        device.endFrame();
        destroyDevice(device);
    }

    //==================================================================================================================
    RenderPassDesc fullPass(std::span<const ColorAttachment> colors)
    {
        return RenderPassDesc{ .colors = colors, .area = { 0, 0, PassKit::kSize, PassKit::kSize }, .label = "kit.pass" };
    }

    //==================================================================================================================
    Access accessesOf(Layout layout)
    {
        switch (layout)
        {
        case Layout::General: return Access::ShaderRead | Access::CopyRead | Access::CopyWrite;
        case Layout::ShaderRead:
        case Layout::DepthStencilRead: return Access::ShaderRead;
        case Layout::ShaderWrite: return Access::ShaderRead | Access::ShaderWrite;
        case Layout::ColorAttachment: return Access::ColorAttachmentRead | Access::ColorAttachmentWrite | Access::Resolve;
        case Layout::DepthStencilWrite: return Access::DepthStencilRead | Access::DepthStencilWrite;
        case Layout::CopySrc: return Access::CopyRead;
        case Layout::CopyDst: return Access::CopyWrite;
        default: return Access::None;
        }
    }

    //==================================================================================================================
    void transition(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, const SubresourceRange& range)
    {
        // A source makes earlier writes available; reads before the barrier need its stage only.
        constexpr Access kWriteAccesses = Access::ShaderWrite | Access::ColorAttachmentWrite | Access::DepthStencilWrite | Access::CopyWrite | Access::Resolve;
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .range = range,
                                                                      .srcStage = Stage::All,
                                                                      .srcAccess = accessesOf(oldLayout) & kWriteAccesses,
                                                                      .dstStage = Stage::All,
                                                                      .dstAccess = accessesOf(newLayout),
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }
} // namespace rojoRHI::test
