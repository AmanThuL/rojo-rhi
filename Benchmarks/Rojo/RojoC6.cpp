//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC6.cpp
/// @brief Implements C6's RojoRHI arm: per repetition dispatch of one group, then a global compute-to-compute barrier.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        constexpr std::array<ShaderEntryName, 1> kEntries{ ShaderEntryName{ "countMain", "compute" } };
        // Frame f's counter sits at block f mod 3: its fill writes the bytes the frame three earlier wrote, which the
        // tracker orders once beginFrame retires that frame's slot (VU-rojo-sync-0021).
        constexpr uint32_t kSlots = 3;
        constexpr uint64_t kCounterBlock = 256;
        constexpr std::array<GlobalBarrier, 1> kFilled{ GlobalBarrier{ .srcStage = Stage::Copy,
                                                                       .srcAccess = Access::CopyWrite,
                                                                       .dstStage = Stage::ComputeShader,
                                                                       .dstAccess = Access::ShaderRead | Access::ShaderWrite } };
        constexpr std::array<GlobalBarrier, 1> kDispatched{ GlobalBarrier{
            .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead } };

        class RojoC6
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC6(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            Pipeline m_pipeline;
            Buffer m_counter;
            Addr m_base = {};
            uint32_t m_lastSlot = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC6::RojoC6(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> RojoC6::create()
        {
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchCompute", kEntries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<Pipeline, std::string> pipeline =
                m_context.pipeline(ComputePipelineDesc{ .compute = ShaderEntry{ *library, "countMain" }, .label = "bench.c6" });
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<Buffer, std::string> counter = m_context.buffer(
                BufferDesc{ .size = kSlots * kCounterBlock, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "bench.c6.counter" }
            );
            if (!counter)
            {
                return std::unexpected(counter.error());
            }
            m_counter = *counter;
            m_base = m_context.device().address(m_counter);
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC6::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Before the repetitions the list zeroes the frame's counter, orders the fill before the dispatches, binds the
        // pipeline and pushes the counter's address once.
        std::expected<FrameResult, std::string> RojoC6::frame(uint32_t index)
        {
            m_lastSlot = index % kSlots;
            const uint64_t offset = m_lastSlot * kCounterBlock;
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c6");
            list.fillBuffer(m_counter, offset, sizeof(uint32_t), 0);
            list.barrier(kFilled);
            list.setPipeline(m_pipeline);
            (void)list.pushRoot(CounterRoot{ .counter = m_base + offset });
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list.dispatch(1, 1, 1);
                list.barrier(kDispatched);
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
        std::expected<std::vector<Output>, std::string> RojoC6::readback()
        {
            const GlobalBarrier counted{
                .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
            };
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_counter, m_lastSlot * kCounterBlock, sizeof(uint32_t), counted);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "counter", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC6(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC6>(config, options);
    }
} // namespace rojoRHI::bench
