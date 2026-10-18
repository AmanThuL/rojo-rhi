//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC4.cpp
/// @brief Implements C4's native arm: per repetition the root into the arena, setAddress and one threadgroup dispatched.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

#include <span>

namespace rojoRHI::bench {
    namespace {
        class NativeC4
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC4(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            MTL::ComputePipelineState* m_pipeline = nullptr;
            MTL::Buffer* m_values = nullptr;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC4::NativeC4(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC4::create()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchCompute");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::ComputePipelineState*, std::string> pipeline = m_context.computePipeline(*library, "storeMain", "bench.c4");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<MTL::Buffer*, std::string> values = m_context.privateBuffer(uint64_t{ m_k } * 4, "bench.c4.values");
            if (!values)
            {
                return std::unexpected(values.error());
            }
            m_values = *values;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = uint64_t{ m_k } * NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC4::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Each dispatch stores to its own element, so the encoder's dispatches, which Metal 4 may run concurrently, need
        // no barrier between them and the output does not depend on their order.
        std::expected<FrameResult, std::string> NativeC4::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            const MTL::GPUAddress values = m_values->gpuAddress();
            const MTL::Size groups = MTL::Size::Make(1, 1, 1);
            const MTL::Size threads = MTL::Size::Make(64, 1, 1);

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(m_pipeline);
            encoder->setArgumentTable(slot.table);
            calls.record += 4;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                const StoreRoot root{ .address = addrOf(values + uint64_t{ repetition } * 4), .value = storeValue(repetition) };
                slot.table->setAddress(slot.push(std::as_bytes(std::span(&root, 1))), 0);
                encoder->dispatchThreadgroups(groups, threads);
                calls.record += 2;
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
        std::expected<std::vector<Output>, std::string> NativeC4::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_values, 0, uint64_t{ m_k } * 4);
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
    ArmResult runNativeC4(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC4>(config, options);
    }
} // namespace rojoRHI::bench
