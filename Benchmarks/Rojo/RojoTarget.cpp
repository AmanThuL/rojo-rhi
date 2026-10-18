//----------------------------------------------------------------------------------------------------------------------
/// @file RojoTarget.cpp
/// @brief Implements the RojoRHI arm's render targets, their setup transition, the target pipeline and target readback.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoTarget.h"

#include "Common/CpuRoots.h"

#include <format>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kReadbackTimeoutNs = 60'000'000'000;
        constexpr std::array<ColorTarget, 1> kColorTargets{ ColorTarget{ .format = Format::RGBA8Unorm } };
        constexpr TextureRegion kTargetRegion{ .extent = Extent3D{ kTargetSize, kTargetSize, 1 } };
        constexpr BufferTextureLayout kTargetRows{ .rowPitch = kTargetSize * 4 };
    } // namespace

    //==================================================================================================================
    std::expected<void, std::string> RojoTarget::create(RojoContext& context, std::string_view label, bool depth)
    {
        m_label = std::string(label);
        m_hasDepth = depth;
        std::array<TextureBarrier, 2 * kSlots> transitions{};
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < kSlots; ++slot)
        {
            const std::string colorLabel = std::format("{}.target.{}", label, slot);
            const std::expected<Texture, std::string> color = context.texture(
                TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kTargetSize, kTargetSize, 1 }, .usage = TextureUsage::RenderTarget, .label = colorLabel }
            );
            if (!color)
            {
                return std::unexpected(color.error());
            }
            m_color[slot] = *color;
            m_attachments[slot][0] = ColorAttachment{ .texture = *color, .load = LoadOp::Clear, .store = StoreOp::Store };
            transitions[count++] = TextureBarrier{ .texture = *color,
                                                   .srcStage = Stage::ColorAttachment,
                                                   .dstStage = Stage::ColorAttachment,
                                                   .dstAccess = Access::ColorAttachmentWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::ColorAttachment };
            if (depth)
            {
                const std::string depthLabel = std::format("{}.depth.{}", label, slot);
                const std::expected<Texture, std::string> depthTexture = context.texture(
                    TextureDesc{
                        .format = Format::D32Float, .extent = { kTargetSize, kTargetSize, 1 }, .usage = TextureUsage::DepthStencil, .label = depthLabel }
                );
                if (!depthTexture)
                {
                    return std::unexpected(depthTexture.error());
                }
                m_depth[slot] = *depthTexture;
                transitions[count++] = TextureBarrier{ .texture = *depthTexture,
                                                       .srcStage = Stage::DepthStencil,
                                                       .dstStage = Stage::DepthStencil,
                                                       .dstAccess = Access::DepthStencilRead | Access::DepthStencilWrite,
                                                       .oldLayout = Layout::Undefined,
                                                       .newLayout = Layout::DepthStencilWrite };
            }
        }

        context.beginFrame(1);
        List list = acquire(context.device(), QueueType::Graphics, std::format("{}.setup", label));
        list.barrier({}, {}, std::span<const TextureBarrier>(transitions.data(), count));
        list.end();
        const std::array<List, 1> lists{ list };
        context.queue(QueueType::Graphics).submit(lists);
        if (const std::expected<std::optional<double>, std::string> ended = context.endFrame(); !ended)
        {
            return std::unexpected(std::format("{} setup: {}", label, ended.error()));
        }
        return {};
    }

    //==================================================================================================================
    RenderPassDesc RojoTarget::pass(uint32_t slot) const
    {
        RenderPassDesc desc{ .colors = m_attachments[slot], .area = Rect2D{ 0, 0, kTargetSize, kTargetSize }, .label = m_label };
        if (m_hasDepth)
        {
            desc.depth = DepthAttachment{ .texture = m_depth[slot], .load = LoadOp::Clear, .store = StoreOp::Store, .clearDepth = 0.0f };
        }
        return desc;
    }

    //==================================================================================================================
    Texture RojoTarget::color(uint32_t slot) const
    {
        return m_color[slot];
    }

    //==================================================================================================================
    std::expected<Pipeline, std::string> RojoTarget::pipeline(
        RojoContext& context,
        ShaderLibrary library,
        std::string_view vertex,
        std::string_view fragment,
        const RasterState& raster,
        const DepthStencilState& depthStencil,
        std::string_view label
    ) const
    {
        return context.pipeline(
            GraphicsPipelineDesc{
                .vertex = ShaderEntry{ library, vertex },
                .fragment = ShaderEntry{ library, fragment },
                .raster = raster,
                .depthStencil = depthStencil,
                .colors = kColorTargets,
                .depthFormat = m_hasDepth ? Format::D32Float : Format::Undefined,
                .label = label,
            }
        );
    }

    //==================================================================================================================
    std::expected<std::vector<Output>, std::string> RojoTarget::read(RojoContext& context, uint32_t slot)
    {
        if (m_readbackFence == Fence{})
        {
            const std::expected<Fence, std::string> created = context.fence(0, std::format("{}.readback", m_label));
            if (!created)
            {
                return std::unexpected(created.error());
            }
            m_readbackFence = *created;
        }
        const uint64_t size = m_hasDepth ? 2 * kTargetBytes : kTargetBytes;
        const std::expected<Buffer, std::string> staging =
            context.buffer(BufferDesc{ .size = size, .memory = Memory::Readback, .label = std::format("{}.readback", m_label) });
        if (!staging)
        {
            return std::unexpected(staging.error());
        }

        std::array<TextureBarrier, 2> toCopy{
            TextureBarrier{ .texture = m_color[slot],
                            .srcStage = Stage::ColorAttachment,
                            .srcAccess = Access::ColorAttachmentWrite,
                            .dstStage = Stage::Copy,
                            .dstAccess = Access::CopyRead,
                            .oldLayout = Layout::ColorAttachment,
                            .newLayout = Layout::CopySrc },
            TextureBarrier{ .texture = m_depth[slot],
                            .srcStage = Stage::DepthStencil,
                            .srcAccess = Access::DepthStencilWrite,
                            .dstStage = Stage::Copy,
                            .dstAccess = Access::CopyRead,
                            .oldLayout = Layout::DepthStencilWrite,
                            .newLayout = Layout::CopySrc },
        };
        context.beginFrame(1);
        List list = acquire(context.device(), QueueType::Graphics, std::format("{}.readback", m_label));
        list.barrier({}, {}, std::span<const TextureBarrier>(toCopy.data(), m_hasDepth ? 2 : 1));
        list.copyTextureToBuffer(m_color[slot], kTargetRegion, *staging, kTargetRows);
        if (m_hasDepth)
        {
            BufferTextureLayout depthRows = kTargetRows;
            depthRows.offset = kTargetBytes;
            list.copyTextureToBuffer(m_depth[slot], kTargetRegion, *staging, depthRows);
        }
        list.end();
        const std::array<List, 1> lists{ list };
        const std::array<FenceValue, 1> signal{ FenceValue{ m_readbackFence, ++m_readbackValue } };
        context.queue(QueueType::Graphics).submit(lists, {}, signal);
        if (const std::expected<std::optional<double>, std::string> ended = context.endFrame(); !ended)
        {
            return std::unexpected("target readback: " + ended.error());
        }
        Device device = context.device();
        if (!device.wait(signal[0], kReadbackTimeoutNs))
        {
            return std::unexpected(std::string("target readback: the fence did not signal within 60 s"));
        }
        const std::byte* bytes = device.cpuPointer(*staging);
        std::vector<Output> outputs;
        outputs.push_back(Output{ "target", std::vector<std::byte>(bytes, bytes + kTargetBytes) });
        if (m_hasDepth)
        {
            outputs.push_back(Output{ "depth", std::vector<std::byte>(bytes + kTargetBytes, bytes + 2 * kTargetBytes) });
        }
        return outputs;
    }
} // namespace rojoRHI::bench
