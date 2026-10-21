//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC2.cpp
/// @brief Implements C2's native arm on D3D12: per repetition the 64-byte root into the arena and an indexed draw.
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
        // Three UInt16 indices, 0, 1 and 2, padded to 8 bytes.
        constexpr std::array<uint16_t, 4> kIndices{ 0, 1, 2, 0 };

        class NativeC2
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC2(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            ID3D12PipelineState* m_pipeline = nullptr;
            D3D12_INDEX_BUFFER_VIEW m_indexView{};
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC2::NativeC2(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        // Every repetition's root starts at its own 256-byte boundary, so the arena holds exactly K of them.
        std::expected<void, std::string> NativeC2::create()
        {
            const std::expected<ID3D12PipelineState*, std::string> pipeline =
                NativeTarget::pipeline(m_context, "BenchDraw", "drawVertex", "drawFragment", "bench.c2", NativeTargetState{});
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            const std::expected<NativeBuffer, std::string> indices =
                m_context.defaultBuffer(sizeof(kIndices), "bench.c2.indices", std::as_bytes(std::span(kIndices)));
            if (!indices)
            {
                return std::unexpected(indices.error());
            }
            m_indexView = D3D12_INDEX_BUFFER_VIEW{ indices->address, static_cast<UINT>(sizeof(kIndices)), DXGI_FORMAT_R16_UINT };
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c2", false); !target)
            {
                return target;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = uint64_t{ m_k } * NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC2::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The root signature, the heaps and the index buffer are bound once with the pipeline state, topology, viewport and
        // scissor; the repetitions change only the root address.
        std::expected<FrameResult, std::string> NativeC2::frame(uint32_t index)
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
            list->SetPipelineState(m_pipeline);
            ++calls.record;
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ++calls.record;
            list->IASetIndexBuffer(&m_indexView);
            ++calls.record;
            list->BeginRenderPass(1, &pass.color, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                const DrawRoot root = drawRoot(repetition);
                list->SetGraphicsRootShaderResourceView(0, slot.push(std::as_bytes(std::span(&root, 1))));
                ++calls.record;
                list->DrawIndexedInstanced(3, 1, 0, 0, 0);
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
        std::expected<std::vector<Output>, std::string> NativeC2::readback()
        {
            return m_target.read(m_context, m_lastIndex % NativeContext::kFrameSlots);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC2(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC2>(config, options);
    }
} // namespace rojoRHI::bench
