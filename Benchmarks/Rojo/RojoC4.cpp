//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC4.cpp
/// @brief Implements C4's RojoRHI arm: per repetition pushRoot of the store's root, then dispatch of one group.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        constexpr std::array<ShaderEntryName, 1> kEntries{ ShaderEntryName{ "storeMain", "compute" } };

        class RojoC4
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC4(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            Pipeline m_pipeline;
            Buffer m_values;
            Addr m_base = {};
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC4::RojoC4(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> RojoC4::create()
        {
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchCompute", kEntries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<Pipeline, std::string> pipeline =
                m_context.pipeline(ComputePipelineDesc{ .compute = ShaderEntry{ *library, "storeMain" }, .label = "bench.c4" });
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<Buffer, std::string> values = m_context.buffer(
                BufferDesc{ .size = uint64_t{ m_k } * 4, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "bench.c4.values" }
            );
            if (!values)
            {
                return std::unexpected(values.error());
            }
            m_values = *values;
            m_base = m_context.device().address(m_values);
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC4::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Each dispatch stores to its own element, so no barrier orders the dispatches and the output does not depend on
        // their order.
        std::expected<FrameResult, std::string> RojoC4::frame(uint32_t /*index*/)
        {
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c4");
            list.setPipeline(m_pipeline);
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                (void)list.pushRoot(StoreRoot{ .address = m_base + uint64_t{ repetition } * 4, .value = storeValue(repetition) });
                list.dispatch(1, 1, 1);
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
        std::expected<std::vector<Output>, std::string> RojoC4::readback()
        {
            const GlobalBarrier stored{
                .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
            };
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_values, 0, uint64_t{ m_k } * 4, stored);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "values", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC4(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC4>(config, options);
    }
} // namespace rojoRHI::bench
