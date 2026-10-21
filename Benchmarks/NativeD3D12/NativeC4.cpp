//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC4.cpp
/// @brief Implements C4's native arm on D3D12: per repetition the 16-byte root into the arena and a dispatch.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

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
            ID3D12PipelineState* m_pipeline = nullptr;
            NativeBuffer m_values;
            uint32_t m_view = 0; ///< The view-heap index of the values buffer's raw views, the low word of its Addr.
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC4::NativeC4(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC4::create()
        {
            const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = m_context.library("BenchCompute", "storeMain");
            if (!shader)
            {
                return std::unexpected(shader.error());
            }
            const std::expected<ID3D12PipelineState*, std::string> pipeline = m_context.computePipeline(*shader, "bench.c4");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<NativeBuffer, std::string> values = m_context.defaultBuffer(uint64_t{ m_k } * 4, "bench.c4.values");
            if (!values)
            {
                return std::unexpected(values.error());
            }
            m_values = *values;
            const std::expected<uint32_t, std::string> view = m_context.bufferViews(m_values.resource);
            if (!view)
            {
                return std::unexpected(view.error());
            }
            m_view = *view;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = uint64_t{ m_k } * NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC4::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Each dispatch stores to its own element, so no barrier orders the dispatches and the output does not depend on
        // their order.
        std::expected<FrameResult, std::string> NativeC4::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];

            const uint64_t start = hostTicks();
            if (const std::expected<void, std::string> reset = resetList(m_context, slot, 0); !reset)
            {
                return std::unexpected(reset.error());
            }
            list->SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
            ++calls.record;
            list->SetComputeRootSignature(m_context.rootSignature());
            ++calls.record;
            list->SetPipelineState(m_pipeline);
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                const StoreRoot root{ .address = Addr{ m_view, repetition * 4 }, .value = storeValue(repetition) };
                list->SetComputeRootShaderResourceView(0, slot.push(std::as_bytes(std::span(&root, 1))));
                ++calls.record;
                list->Dispatch(1, 1, 1);
                ++calls.record;
            }
            if (const std::expected<void, std::string> closed = closeList(m_context, list); !closed)
            {
                return std::unexpected(closed.error());
            }
            const uint64_t end = hostTicks();

            ID3D12CommandList* const lists[] = { list };
            m_context.commit(m_context.graphicsQueue(), lists);
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
            std::expected<std::vector<std::byte>, std::string> bytes = readBufferAfter(
                m_context, m_values.resource, 0, uint64_t{ m_k } * 4, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
            );
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
