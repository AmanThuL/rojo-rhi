//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTarget.cpp
/// @brief Implements the native arm's render targets, their pass descriptors, the target pipeline and target readback.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeTarget.h"

#include "Common/CpuRoots.h"

#include <format>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        std::expected<MTL::Texture*, std::string> targetTexture(NativeContext& context, MTL::PixelFormat format, std::string_view label)
        {
            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setPixelFormat(format);
            descriptor->setWidth(kTargetSize);
            descriptor->setHeight(kTargetSize);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            descriptor->setUsage(MTL::TextureUsageRenderTarget);
            std::expected<MTL::Texture*, std::string> texture = context.texture(descriptor, label);
            descriptor->release();
            return texture;
        }
    } // namespace

    //==================================================================================================================
    std::expected<void, std::string> NativeTarget::create(NativeContext& context, std::string_view label, bool depth)
    {
        for (uint32_t slot = 0; slot < NativeContext::kFrameSlots; ++slot)
        {
            const std::expected<MTL::Texture*, std::string> color =
                targetTexture(context, MTL::PixelFormatRGBA8Unorm, std::format("{}.target.{}", label, slot));
            if (!color)
            {
                return std::unexpected(color.error());
            }
            m_color[slot] = *color;

            MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
            context.own(pass);
            m_passes[slot] = pass;
            MTL::RenderPassColorAttachmentDescriptor* attachment = pass->colorAttachments()->object(0);
            attachment->setTexture(m_color[slot]);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 0.0));

            if (depth)
            {
                const std::expected<MTL::Texture*, std::string> depthTexture =
                    targetTexture(context, MTL::PixelFormatDepth32Float, std::format("{}.depth.{}", label, slot));
                if (!depthTexture)
                {
                    return std::unexpected(depthTexture.error());
                }
                m_depth[slot] = *depthTexture;
                MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = pass->depthAttachment();
                depthAttachment->setTexture(m_depth[slot]);
                depthAttachment->setLoadAction(MTL::LoadActionClear);
                depthAttachment->setStoreAction(MTL::StoreActionStore);
                depthAttachment->setClearDepth(0.0);
            }
        }
        return {};
    }

    //==================================================================================================================
    MTL4::RenderPassDescriptor* NativeTarget::pass(uint32_t slot) const
    {
        return m_passes[slot];
    }

    //==================================================================================================================
    std::expected<MTL::RenderPipelineState*, std::string> NativeTarget::pipeline(
        NativeContext& context, MTL::Library* library, std::string_view vertex, std::string_view fragment, std::string_view label
    )
    {
        MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
        descriptor->setVertexFunctionDescriptor(context.function(library, vertex));
        descriptor->setFragmentFunctionDescriptor(context.function(library, fragment));
        descriptor->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        std::expected<MTL::RenderPipelineState*, std::string> pipeline = context.renderPipeline(descriptor, label);
        descriptor->release();
        return pipeline;
    }

    //==================================================================================================================
    std::expected<std::vector<Output>, std::string> NativeTarget::read(NativeContext& context, uint32_t frameIndex, uint32_t slot) const
    {
        MTL::Texture* depth = m_depth[slot];
        const uint64_t size = depth != nullptr ? 2 * kTargetBytes : kTargetBytes;
        const std::expected<MTL::Buffer*, std::string> staging = context.sharedBuffer(size, "bench.readback.target");
        if (!staging)
        {
            return std::unexpected(staging.error());
        }
        NativeCalls& calls = context.calls();
        context.residency()->commit();
        ++calls.host;

        // The frames that rendered the slot have completed, so their attachment stores are visible to this copy.
        NativeContext::Slot& frame = context.beginFrame(frameIndex, 1);
        MTL4::CommandBuffer* commands = frame.lists[0];
        commands->beginCommandBuffer(frame.allocators[0]);
        MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
        const MTL::Size extent = MTL::Size::Make(kTargetSize, kTargetSize, 1);
        encoder->copyFromTexture(m_color[slot], 0, 0, MTL::Origin::Make(0, 0, 0), extent, *staging, 0, uint64_t{ kTargetSize } * 4, kTargetBytes);
        calls.record += 3;
        if (depth != nullptr)
        {
            encoder->copyFromTexture(depth, 0, 0, MTL::Origin::Make(0, 0, 0), extent, *staging, kTargetBytes, uint64_t{ kTargetSize } * 4, kTargetBytes);
            ++calls.record;
        }
        encoder->endEncoding();
        commands->endCommandBuffer();
        calls.record += 2;
        MTL4::CommandBuffer* const buffers[] = { commands };
        context.commit(context.graphicsQueue(), buffers);
        if (const std::expected<std::optional<double>, std::string> ended = context.endFrame(); !ended)
        {
            return std::unexpected("target readback: " + ended.error());
        }

        const auto* bytes = static_cast<const std::byte*>((*staging)->contents());
        std::vector<Output> outputs;
        outputs.push_back(Output{ "target", std::vector<std::byte>(bytes, bytes + kTargetBytes) });
        if (depth != nullptr)
        {
            outputs.push_back(Output{ "depth", std::vector<std::byte>(bytes + kTargetBytes, bytes + 2 * kTargetBytes) });
        }
        return outputs;
    }
} // namespace rojoRHI::bench
