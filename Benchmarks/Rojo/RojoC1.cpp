//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC1.cpp
/// @brief Implements C1's RojoRHI arm: per repetition pushRoot of the 64-byte root, then draw of 3 vertices.
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
        constexpr std::array<ShaderEntryName, 2> kEntries{ ShaderEntryName{ "drawVertex", "vertex" }, ShaderEntryName{ "drawFragment", "fragment" } };

        class RojoC1
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC1(RojoContext&, const Config&);
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
        RojoC1::RojoC1(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> RojoC1::create()
        {
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchDraw", kEntries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c1", false); !target)
            {
                return target;
            }
            const std::expected<Pipeline, std::string> pipeline = m_target.pipeline(m_context, *library, "drawVertex", "drawFragment", {}, {}, "bench.c1");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC1::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoC1::frame(uint32_t index)
        {
            m_lastSlot = index % RojoTarget::kSlots;
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c1");
            list.beginRenderPass(m_target.pass(m_lastSlot));
            list.setPipeline(m_pipeline);
            list.setViewport(RojoTarget::kViewport);
            list.setScissor(RojoTarget::kScissor);
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                (void)list.pushRoot(drawRoot(repetition));
                list.draw(3);
            }
            list.endRenderPass();
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
        std::expected<std::vector<Output>, std::string> RojoC1::readback()
        {
            return m_target.read(m_context, m_lastSlot);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC1(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC1>(config, options);
    }
} // namespace rojoRHI::bench
