//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC3.cpp
/// @brief Implements C3's native arm on D3D12: per repetition setPipelineState and a draw, alternating two pipelines.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        // A: cull None, no depth test or write; B: cull Back, depth test Greater with writes, as the RojoRHI arm's two
        // pipelines. Cull mode and depth-stencil state live in the pipeline state, so a pipeline change is the only call.
        constexpr std::array<NativeTargetState, 2> kStates{
            NativeTargetState{ .depth = true },
            NativeTargetState{
                .cull = D3D12_CULL_MODE_BACK, .depth = true, .depthTest = true, .depthWrite = true, .depthCompare = D3D12_COMPARISON_FUNC_GREATER },
        };

        class NativeC3
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC3(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            std::array<ID3D12PipelineState*, 2> m_pipelines{};
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC3::NativeC3(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC3::create()
        {
            for (uint32_t index = 0; index < 2; ++index)
            {
                const std::expected<ID3D12PipelineState*, std::string> pipeline =
                    NativeTarget::pipeline(m_context, "BenchPlain", "plainVertex", "plainFragment", index == 0 ? "bench.c3.a" : "bench.c3.b", kStates[index]);
                if (!pipeline)
                {
                    return std::unexpected(pipeline.error());
                }
                m_pipelines[index] = *pipeline;
            }
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c3", true); !target)
            {
                return target;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC3::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Both pipelines share a topology and the pass's shaders read no root, so the root signature, topology, viewport and
        // scissor are set once and the repetitions change only the pipeline state.
        std::expected<FrameResult, std::string> NativeC3::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            const NativePass& pass = m_target.pass(index % NativeContext::kFrameSlots);
            m_lastIndex = index;

            const uint64_t start = hostTicks();
            if (const std::expected<void, std::string> reset = resetList(m_context, slot, 0); !reset)
            {
                return std::unexpected(reset.error());
            }
            list->SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
            ++calls.record;
            list->SetGraphicsRootSignature(m_context.rootSignature());
            ++calls.record;
            list->RSSetViewports(1, &NativeTarget::kViewport);
            ++calls.record;
            list->RSSetScissorRects(1, &NativeTarget::kScissor);
            ++calls.record;
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ++calls.record;
            list->BeginRenderPass(1, &pass.color, &pass.depth, D3D12_RENDER_PASS_FLAG_NONE);
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list->SetPipelineState(m_pipelines[repetition & 1]);
                ++calls.record;
                list->DrawInstanced(3, 1, 0, 0);
                ++calls.record;
            }
            list->EndRenderPass();
            ++calls.record;
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
        std::expected<std::vector<Output>, std::string> NativeC3::readback()
        {
            return m_target.read(m_context, m_lastIndex % NativeContext::kFrameSlots);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC3(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC3>(config, options);
    }
} // namespace rojoRHI::bench
