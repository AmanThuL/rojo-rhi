//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC6.cpp
/// @brief Implements C6's native arm on D3D12: per repetition a dispatch and a global barrier before the next.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

#include <array>
#include <span>

namespace rojoRHI::bench {
    namespace {
        // The counter of frame f sits at block f mod 3, as the RojoRHI arm's does.
        constexpr uint64_t kCounterBlock = 256;

        // The copy that zeroes a frame's counter, ordered before the first dispatch, which reads and writes it.
        constexpr D3D12_GLOBAL_BARRIER kFilled{ D3D12_BARRIER_SYNC_COPY,
                                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                                D3D12_BARRIER_ACCESS_COPY_DEST,
                                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
        // Chapter 09's lowering of a compute write ordered before the next dispatch's read: the dispatch reads the counter
        // through its shader-resource view and writes it through its unordered-access view.
        constexpr D3D12_GLOBAL_BARRIER kDispatched{ D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };

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
            ID3D12PipelineState* m_pipeline = nullptr;
            NativeBuffer m_counter;
            NativeBuffer m_zero; ///< Four zero bytes, copied over a frame's counter.
            uint32_t m_view = 0; ///< The view-heap index of the counter's raw views, the low word of its Addr.
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
            const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = m_context.library("BenchCompute", "countMain");
            if (!shader)
            {
                return std::unexpected(shader.error());
            }
            const std::expected<ID3D12PipelineState*, std::string> pipeline = m_context.computePipeline(*shader, "bench.c6");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<NativeBuffer, std::string> counter = m_context.defaultBuffer(NativeContext::kFrameSlots * kCounterBlock, "bench.c6.counter");
            if (!counter)
            {
                return std::unexpected(counter.error());
            }
            m_counter = *counter;
            const std::array<std::byte, sizeof(uint32_t)> zero{};
            const std::expected<NativeBuffer, std::string> source = m_context.uploadBuffer(zero.size(), "bench.c6.zero", zero);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_zero = *source;
            const std::expected<uint32_t, std::string> view = m_context.bufferViews(m_counter.resource);
            if (!view)
            {
                return std::unexpected(view.error());
            }
            m_view = *view;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC6::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Before the repetitions the frame zeroes its counter with a copy and orders the copy before the first dispatch,
        // binds the pipeline and sets the root's address once. Each repetition's barrier orders the next dispatch after the
        // last one's write.
        std::expected<FrameResult, std::string> NativeC6::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            m_lastSlot = index % NativeContext::kFrameSlots;
            const uint64_t offset = m_lastSlot * kCounterBlock;
            const CounterRoot root{ .counter = Addr{ m_view, static_cast<uint32_t>(offset) } };
            D3D12_BARRIER_GROUP filled{};
            filled.Type = D3D12_BARRIER_TYPE_GLOBAL;
            filled.NumBarriers = 1;
            filled.pGlobalBarriers = &kFilled;
            D3D12_BARRIER_GROUP dispatched{};
            dispatched.Type = D3D12_BARRIER_TYPE_GLOBAL;
            dispatched.NumBarriers = 1;
            dispatched.pGlobalBarriers = &kDispatched;

            const uint64_t start = hostTicks();
            if (const std::expected<void, std::string> reset = resetList(m_context, slot, 0); !reset)
            {
                return std::unexpected(reset.error());
            }
            list->CopyBufferRegion(m_counter.resource, offset, m_zero.resource, 0, sizeof(uint32_t));
            ++calls.record;
            list->Barrier(1, &filled);
            ++calls.record;
            ++calls.barriers;
            list->SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
            ++calls.record;
            list->SetComputeRootSignature(m_context.rootSignature());
            ++calls.record;
            list->SetPipelineState(m_pipeline);
            ++calls.record;
            list->SetComputeRootShaderResourceView(0, slot.push(std::as_bytes(std::span(&root, 1))));
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list->Dispatch(1, 1, 1);
                ++calls.record;
                list->Barrier(1, &dispatched);
                ++calls.record;
                ++calls.barriers;
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
        std::expected<std::vector<Output>, std::string> NativeC6::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = readBufferAfter(
                m_context,
                m_counter.resource,
                m_lastSlot * kCounterBlock,
                sizeof(uint32_t),
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
            );
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
