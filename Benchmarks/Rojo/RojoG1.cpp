//----------------------------------------------------------------------------------------------------------------------
/// @file RojoG1.cpp
/// @brief Implements G1's RojoRHI arm: one dispatch of 2^20 threads, each summing 16 uint4 loads through an Addr from the root.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        // Frames in flight; frame f writes region f mod 3 of the output.
        constexpr uint32_t kRegions = 3;
        constexpr uint64_t kRegionBytes = uint64_t{ kKernelThreads } * sizeof(DataEntry);

        // The root BenchLoads.slang declares.
        struct LoadsRoot
        {
            Addr data;
            Addr output;
            uint32_t divergent;
            uint32_t pad;
        };

        class RojoG1
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoG1(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            bool m_divergent = false;
            Buffer m_data;
            Buffer m_output;
            Pipeline m_pipeline;
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoG1::RojoG1(RojoContext& context, const Config& config)
            : m_context(context), m_divergent(config.pattern.value_or("") == "divergent"), m_ready(create())
        {
        }

        //==============================================================================================================
        // The output holds one region per frame slot, so no two frames write the same bytes: the validation tracker
        // reports two writes to one range with no barrier between them (VU-rojo-sync-0021).
        std::expected<void, std::string> RojoG1::create()
        {
            const std::vector<DataEntry> table = dataTable();
            const std::expected<Buffer, std::string> data = m_context.buffer(
                BufferDesc{ .size = table.size() * sizeof(DataEntry), .memory = Memory::Gpu, .label = "bench.g1.data" }, std::as_bytes(std::span(table))
            );
            if (!data)
            {
                return std::unexpected(data.error());
            }
            m_data = *data;
            const std::expected<Buffer, std::string> output =
                m_context.buffer(BufferDesc{ .size = kRegions * kRegionBytes, .memory = Memory::Gpu, .label = "bench.g1.output" });
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;

            const std::array<ShaderEntryName, 1> entries{ ShaderEntryName{ "loadsMain", "compute" } };
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchLoads", entries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<Pipeline, std::string> pipeline =
                m_context.pipeline(ComputePipelineDesc{ .compute = ShaderEntry{ .library = *library, .name = "loadsMain" }, .label = "bench.g1" });
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoG1::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoG1::frame(uint32_t index)
        {
            m_lastRegion = index % kRegions;
            const LoadsRoot root{
                .data = m_context.device().address(m_data),
                .output = m_context.device().address(m_output, m_lastRegion * kRegionBytes),
                .divergent = m_divergent ? 1u : 0u,
                .pad = 0,
            };
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.g1");
            list.setPipeline(m_pipeline);
            list.pushRoot(root);
            list.dispatch(kKernelThreads / kKernelGroup, 1, 1);
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
        std::expected<std::vector<Output>, std::string> RojoG1::readback()
        {
            const GlobalBarrier written{
                .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
            };
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_output, m_lastRegion * kRegionBytes, kRegionBytes, written);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "output", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoG1(const Config& config, const RunOptions& options)
    {
        return runArm<RojoG1>(config, options);
    }
} // namespace rojoRHI::bench
