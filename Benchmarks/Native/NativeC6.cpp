//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC6.cpp
/// @brief Implements C6's native arm: per repetition dispatchThreadgroups, then a dispatch-to-dispatch stage barrier.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

#include <span>

namespace rojoRHI::bench {
    namespace {
        // The counter of frame f sits at block f mod 3, as the RojoRHI arm's does.
        constexpr uint64_t kCounterBlock = 256;

        class NativeC6
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC6(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            MTL::ComputePipelineState* m_pipeline = nullptr;
            MTL::Buffer* m_counter = nullptr;
            uint32_t m_lastSlot = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC6::NativeC6(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC6::create()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchCompute");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::ComputePipelineState*, std::string> pipeline = m_context.computePipeline(*library, "countMain", "bench.c6");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<MTL::Buffer*, std::string> counter = m_context.privateBuffer(NativeContext::kFrameSlots * kCounterBlock, "bench.c6.counter");
            if (!counter)
            {
                return std::unexpected(counter.error());
            }
            m_counter = *counter;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC6::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Before the repetitions the frame zeroes its counter and orders the fill before the first dispatch, binds the
        // pipeline and the table and sets the root's address once. Each repetition's barrier is chapter 09's lowering of
        // a global ComputeShader/ShaderWrite to ComputeShader/ShaderRead barrier inside one compute encoder.
        std::expected<FrameResult, std::string> NativeC6::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            m_lastSlot = index % NativeContext::kFrameSlots;
            const uint64_t offset = m_lastSlot * kCounterBlock;
            const CounterRoot root{ .counter = addrOf(m_counter->gpuAddress() + offset) };
            const MTL::Size groups = MTL::Size::Make(1, 1, 1);
            const MTL::Size threads = MTL::Size::Make(64, 1, 1);

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            encoder->fillBuffer(m_counter, NS::Range::Make(offset, sizeof(uint32_t)), 0);
            encoder->barrierAfterEncoderStages(MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
            encoder->setComputePipelineState(m_pipeline);
            encoder->setArgumentTable(slot.table);
            slot.table->setAddress(slot.push(std::as_bytes(std::span(&root, 1))), 0);
            calls.record += 7;
            ++calls.barriers;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                encoder->dispatchThreadgroups(groups, threads);
                encoder->barrierAfterEncoderStages(MTL::StageDispatch, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
                calls.record += 2;
                ++calls.barriers;
            }
            encoder->endEncoding();
            commands->endCommandBuffer();
            calls.record += 2;
            const uint64_t end = hostTicks();

            MTL4::CommandBuffer* const buffers[] = { commands };
            m_context.commit(m_context.graphicsQueue(), buffers);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeC6::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_counter, m_lastSlot * kCounterBlock, sizeof(uint32_t));
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
    ArmResult runNativeC6(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC6>(config, options);
    }
} // namespace rojoRHI::bench
