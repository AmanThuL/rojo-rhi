//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC3.cpp
/// @brief Implements C3's RojoRHI arm: per repetition setPipeline alternating pipelines A and B, then draw of 3 vertices.
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

        class RojoC3
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC3(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            RojoTarget m_target;
            std::array<Pipeline, 2> m_pipelines{};
            uint32_t m_lastSlot = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC3::RojoC3(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        // A: cull None, no depth test; B: cull Back, depth test Greater with writes. A depth test needs a depth
        // attachment (VU-rojo-pipeline-0011), so the pass has the D32Float target, cleared to 0.
        std::expected<void, std::string> RojoC3::create()
        {
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchPlain", kEntries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c3", true); !target)
            {
                return target;
            }
            const std::expected<Pipeline, std::string> a = m_target.pipeline(
                m_context, *library, "plainVertex", "plainFragment", RasterState{ .cull = CullMode::None }, DepthStencilState{}, "bench.c3.a"
            );
            if (!a)
            {
                return std::unexpected(a.error());
            }
            m_pipelines[0] = *a;
            const std::expected<Pipeline, std::string> b = m_target.pipeline(
                m_context,
                *library,
                "plainVertex",
                "plainFragment",
                RasterState{ .cull = CullMode::Back },
                DepthStencilState{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Greater },
                "bench.c3.b"
            );
            if (!b)
            {
                return std::unexpected(b.error());
            }
            m_pipelines[1] = *b;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC3::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoC3::frame(uint32_t index)
        {
            m_lastSlot = index % RojoTarget::kSlots;
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c3");
            list.beginRenderPass(m_target.pass(m_lastSlot));
            list.setViewport(RojoTarget::kViewport);
            list.setScissor(RojoTarget::kScissor);
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list.setPipeline(m_pipelines[repetition & 1]);
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
        std::expected<std::vector<Output>, std::string> RojoC3::readback()
        {
            return m_target.read(m_context, m_lastSlot);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC3(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC3>(config, options);
    }
} // namespace rojoRHI::bench
