//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC8.cpp
/// @brief Implements C8's native arm on D3D12: per repetition a barrier, an event, two timestamps and one render pass.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

#include <array>
#include <format>

namespace rojoRHI::bench {
    namespace {
        // The label of every pass's event, a null-terminated UTF-16 string: the text RojoC8 gives its render pass (the target's label,
        // "bench.c8"), which the backend's scope lowering passes to BeginEvent.
        constexpr wchar_t kEventLabel[] = L"bench.c8";
        constexpr UINT kUnicodeEvent = 0;

        class NativeC8
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC8(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            std::expected<void, std::string> checkTimestamps(uint32_t);

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            ID3D12PipelineState* m_pipeline = nullptr;
            // Per frame slot, a timestamp heap of 2K queries (pass i writes 2i and 2i + 1) and the READBACK buffer its
            // frame resolves them into.
            std::array<ID3D12QueryHeap*, NativeContext::kFrameSlots> m_heaps{};
            std::array<NativeBuffer, NativeContext::kFrameSlots> m_resolved{};
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC8::NativeC8(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC8::create()
        {
            if (m_k == 0)
            {
                return std::unexpected(std::string("bench.c8: a frame holds at least one pass"));
            }
            const std::expected<ID3D12PipelineState*, std::string> pipeline =
                NativeTarget::pipeline(m_context, "BenchPlain", "plainVertex", "plainFragment", "bench.c8", NativeTargetState{});
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c8", false); !target)
            {
                return target;
            }
            const uint32_t queries = 2 * m_k;
            for (uint32_t slot = 0; slot < NativeContext::kFrameSlots; ++slot)
            {
                const std::expected<ID3D12QueryHeap*, std::string> heap = m_context.timestampHeap(queries, std::format("bench.c8.timestamps.{}", slot));
                if (!heap)
                {
                    return std::unexpected(heap.error());
                }
                m_heaps[slot] = *heap;
                const std::expected<NativeBuffer, std::string> resolved =
                    m_context.readbackBuffer(uint64_t{ queries } * sizeof(uint64_t), std::format("bench.c8.resolved.{}", slot));
                if (!resolved)
                {
                    return std::unexpected(resolved.error());
                }
                m_resolved[slot] = *resolved;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC8::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The root signature, heaps, pipeline state, topology, viewport and scissor persist across the passes of one list,
        // so they are set before the first. Every pass but the first opens with a barrier from the previous pass's writes
        // of the target to this pass's. One ResolveQueryData per list, outside the repetitions, copies all 2K timestamps.
        std::expected<FrameResult, std::string> NativeC8::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            const uint32_t slotIndex = index % NativeContext::kFrameSlots;
            m_lastIndex = index;
            const NativePass& pass = m_target.pass(slotIndex);
            ID3D12QueryHeap* heap = m_heaps[slotIndex];
            const D3D12_TEXTURE_BARRIER betweenPasses = m_target.passBarrier(slotIndex);
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &betweenPasses;

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
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                if (repetition > 0)
                {
                    list->Barrier(1, &group);
                    ++calls.record;
                    ++calls.barriers;
                }
                list->BeginEvent(kUnicodeEvent, kEventLabel, static_cast<UINT>(sizeof(kEventLabel)));
                ++calls.record;
                list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * repetition);
                ++calls.record;
                list->BeginRenderPass(1, &pass.color, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
                ++calls.record;
                list->DrawInstanced(3, 1, 0, 0);
                ++calls.record;
                list->EndRenderPass();
                ++calls.record;
                list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * repetition + 1);
                ++calls.record;
                list->EndEvent();
                ++calls.record;
            }
            list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * m_k, m_resolved[slotIndex].resource, 0);
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
        // The slot's last frame has completed by now, so its resolved timestamps are final. Every one of the 2K entries was
        // written, so none is zero, and each follows the one before it in recording order.
        std::expected<void, std::string> NativeC8::checkTimestamps(uint32_t slot)
        {
            const NativeBuffer& resolved = m_resolved[slot];
            NativeCalls& calls = m_context.calls();
            const uint32_t queries = 2 * m_k;
            const D3D12_RANGE read{ 0, static_cast<SIZE_T>(resolved.size) };
            void* mapped = nullptr;
            const HRESULT mappedResult = resolved.resource->Map(0, &read, &mapped);
            ++calls.host;
            if (FAILED(mappedResult))
            {
                return std::unexpected(std::format("bench.c8: Map of the resolved timestamps failed (0x{:08X})", static_cast<uint32_t>(mappedResult)));
            }
            const auto* ticks = static_cast<const uint64_t*>(mapped);
            std::expected<void, std::string> checked;
            for (uint32_t query = 0; query < queries && checked; ++query)
            {
                if (ticks[query] == 0)
                {
                    checked = std::unexpected(std::format("bench.c8: timestamp {} of {} resolved to zero", query, queries));
                }
                else if (query > 0 && ticks[query] < ticks[query - 1])
                {
                    checked = std::unexpected(
                        std::format(
                            "bench.c8: timestamp {} of {} is {} ticks, before timestamp {} at {} ticks",
                            query,
                            queries,
                            ticks[query],
                            query - 1,
                            ticks[query - 1]
                        )
                    );
                }
            }
            const D3D12_RANGE written{ 0, 0 };
            resolved.resource->Unmap(0, &written);
            ++calls.host;
            return checked;
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeC8::readback()
        {
            const uint32_t slot = m_lastIndex % NativeContext::kFrameSlots;
            std::expected<std::vector<Output>, std::string> outputs = m_target.read(m_context, slot);
            if (!outputs)
            {
                return outputs;
            }
            if (const std::expected<void, std::string> stamped = checkTimestamps(slot); !stamped)
            {
                return std::unexpected(stamped.error());
            }
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC8(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC8>(config, options);
    }
} // namespace rojoRHI::bench
