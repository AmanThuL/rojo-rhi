//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC8.cpp
/// @brief Implements C8's RojoRHI arm: per repetition one render pass holding setPipeline, viewport, scissor and a draw.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"
#include "Rojo/RojoTarget.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        constexpr std::array<ShaderEntryName, 2> kEntries{ ShaderEntryName{ "plainVertex", "vertex" }, ShaderEntryName{ "plainFragment", "fragment" } };

        class RojoC8
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC8(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            RojoTarget m_target;
            Pipeline m_pipeline;
            uint32_t m_lastSlot = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC8::RojoC8(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> RojoC8::create()
        {
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchPlain", kEntries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c8", false); !target)
            {
                return target;
            }
            const std::expected<Pipeline, std::string> pipeline = m_target.pipeline(m_context, *library, "plainVertex", "plainFragment", {}, {}, "bench.c8");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC8::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Every pass clears and draws the frame's target, so a barrier orders each pass's attachment write after the
        // previous pass's (VU-rojo-sync-0021); none precedes the first pass.
        std::expected<FrameResult, std::string> RojoC8::frame(uint32_t index)
        {
            m_lastSlot = index % RojoTarget::kSlots;
            const RenderPassDesc pass = m_target.pass(m_lastSlot);
            const std::array<TextureBarrier, 1> betweenPasses{ TextureBarrier{ .texture = m_target.color(m_lastSlot),
                                                                               .srcStage = Stage::ColorAttachment,
                                                                               .srcAccess = Access::ColorAttachmentWrite,
                                                                               .dstStage = Stage::ColorAttachment,
                                                                               .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                                               .oldLayout = Layout::ColorAttachment,
                                                                               .newLayout = Layout::ColorAttachment } };
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c8");
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                if (repetition > 0)
                {
                    list.barrier({}, {}, betweenPasses);
                }
                list.beginRenderPass(pass);
                list.setPipeline(m_pipeline);
                list.setViewport(RojoTarget::kViewport);
                list.setScissor(RojoTarget::kScissor);
                list.draw(3);
                list.endRenderPass();
            }
            list.end();
            const uint64_t end = hostTicks();
            const std::array<List, 1> lists{ list };
            m_context.queue(QueueType::Graphics).submit(lists);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> RojoC8::readback()
        {
            return m_target.read(m_context, m_lastSlot);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC8(const Config& config, const RunOptions& options)
    {
        // N writes the debug group and the two timestamps of every pass, so R times its passes too.
        RunOptions timed = options;
        timed.scopeTimings = true;
        return runArm<RojoC8>(config, timed);
    }
} // namespace rojoRHI::bench
