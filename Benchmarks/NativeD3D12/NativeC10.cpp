//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC10.cpp
/// @brief Implements C10's native arm on D3D12: K counted draws, one ExecuteIndirect each, in one render pass over a 256 by 256 R32Float target.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CountedArms.h"
#include "Common/CountedData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeIndirect.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        std::string failedCall(std::string_view what, HRESULT result)
        {
            return std::format("C10: {} failed with HRESULT 0x{:08X}", what, static_cast<uint32_t>(result));
        }

        // C10 runs counted draws of maxCount records under a count buffer holding maxCount; C10m1 runs one record with no
        // count buffer. Both are one ExecuteIndirect per draw on the draw signature, whose CONSTANT argument writes each
        // record's drawId to b1. A configuration whose pattern is "indexed" draws UInt16 indexed records instead of C10's;
        // no registered row does, and a check does.
        class NativeC10
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC10(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            std::expected<void, std::string> createPipeline();
            std::expected<void, std::string> createTarget();
            std::expected<void, std::string> createRecords();

            NativeContext& m_context;
            NativeIndirect m_indirect;
            uint32_t m_k = 0;
            uint32_t m_maxCount = 0;
            bool m_nullCount = false;
            bool m_indexed = false;
            ID3D12Resource* m_target = nullptr;
            ID3D12PipelineState* m_pipeline = nullptr;
            D3D12_RENDER_PASS_RENDER_TARGET_DESC m_color{};
            D3D12_TEXTURE_BARRIER m_discard{};
            D3D12_INDEX_BUFFER_VIEW m_indices{};
            CountedDraw m_draw{};
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC10::NativeC10(NativeContext& context, const Config& config)
            : m_context(context), m_indirect(context), m_k(config.k.value_or(0)), m_maxCount(config.maxCount.value_or(kCountedMaxCount)),
              m_nullCount(config.nullCount.value_or(false)), m_indexed(config.pattern == "indexed"), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC10::createPipeline()
        {
            const std::expected<D3D12_SHADER_BYTECODE, std::string> vertex = m_context.library("BenchPoints", "pointVertex");
            if (!vertex)
            {
                return std::unexpected(vertex.error());
            }
            const std::expected<D3D12_SHADER_BYTECODE, std::string> pixel = m_context.library("BenchPoints", "pointFragment");
            if (!pixel)
            {
                return std::unexpected(pixel.error());
            }
            NativeGraphicsDesc desc;
            desc.vertex = *vertex;
            desc.pixel = *pixel;
            desc.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            desc.colorFormats = { DXGI_FORMAT_R32_FLOAT };
            const std::expected<ID3D12PipelineState*, std::string> pipeline = m_context.graphicsPipeline(desc, "bench.c10.pipeline");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        // The target is created in LAYOUT_RENDER_TARGET with no optimized clear value, as R's is; each frame's pass clears it
        // to 0 and keeps what it draws.
        std::expected<void, std::string> NativeC10::createTarget()
        {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kCountedTargetEdge;
            desc.Height = kCountedTargetEdge;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            const std::expected<ID3D12Resource*, std::string> target = m_context.texture(desc, D3D12_BARRIER_LAYOUT_RENDER_TARGET, "bench.c10.target");
            if (!target)
            {
                return std::unexpected(target.error());
            }
            m_target = *target;
            const std::expected<ID3D12DescriptorHeap*, std::string> heap = m_context.descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, "bench.c10.rtv");
            if (!heap)
            {
                return std::unexpected(heap.error());
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE view = (*heap)->GetCPUDescriptorHandleForHeapStart();
            m_context.device()->CreateRenderTargetView(m_target, nullptr, view);
            m_context.calls().host += 2;

            m_color.cpuDescriptor = view;
            m_color.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            m_color.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R32_FLOAT;
            m_color.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;

            // R's per-frame TextureBarrier from Undefined into ColorAttachment, lowered as chapter 09 lowers it: the target
            // is discarded into LAYOUT_RENDER_TARGET, waiting on every earlier scope.
            m_discard.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            m_discard.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
            m_discard.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
            m_discard.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
            m_discard.LayoutBefore = D3D12_BARRIER_LAYOUT_UNDEFINED;
            m_discard.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
            m_discard.pResource = m_target;
            m_discard.Subresources.IndexOrFirstMipLevel = 0;
            m_discard.Subresources.NumMipLevels = 1;
            m_discard.Subresources.FirstArraySlice = 0;
            m_discard.Subresources.NumArraySlices = 1;
            m_discard.Subresources.FirstPlane = 0;
            m_discard.Subresources.NumPlanes = 1;
            m_discard.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
            return {};
        }

        //==============================================================================================================
        // The records and the count are DEFAULT buffers written once at setup, read by every frame's draws and written by
        // none, so no frame orders them with a barrier. An indexed draw reads one UInt16 index, 0.
        std::expected<void, std::string> NativeC10::createRecords()
        {
            std::vector<std::byte> records;
            if (m_nullCount)
            {
                const DrawIndirectArgs single{ kSingleDrawId, 1, 1, 0, 1000 };
                const DrawIndexedIndirectArgs indexedSingle{ kSingleDrawId, 1, 1, 0, 0, 1000 };
                const std::span<const std::byte> bytes = m_indexed ? std::as_bytes(std::span(&indexedSingle, 1)) : std::as_bytes(std::span(&single, 1));
                records.assign(bytes.begin(), bytes.end());
            }
            else
            {
                records = m_indexed ? indexedRecords(m_maxCount) : drawRecords(m_maxCount);
            }
            const std::expected<NativeBuffer, std::string> recordBuffer = m_context.defaultBuffer(records.size(), "bench.c10.records", records);
            if (!recordBuffer)
            {
                return std::unexpected(recordBuffer.error());
            }
            m_draw = CountedDraw{ .records = recordBuffer->resource, .maxCount = m_maxCount, .indexed = m_indexed };
            if (!m_nullCount)
            {
                const std::array<uint32_t, 1> countValue{ m_maxCount };
                const std::expected<NativeBuffer, std::string> countBuffer =
                    m_context.defaultBuffer(sizeof(uint32_t), "bench.c10.count", std::as_bytes(std::span(countValue)));
                if (!countBuffer)
                {
                    return std::unexpected(countBuffer.error());
                }
                m_draw.count = countBuffer->resource;
            }
            if (m_indexed)
            {
                const std::array<uint16_t, 1> index{ 0 };
                const std::expected<NativeBuffer, std::string> indexBuffer =
                    m_context.defaultBuffer(sizeof(uint16_t), "bench.c10.indices", std::as_bytes(std::span(index)));
                if (!indexBuffer)
                {
                    return std::unexpected(indexBuffer.error());
                }
                m_indices = D3D12_INDEX_BUFFER_VIEW{ indexBuffer->address, static_cast<UINT>(sizeof(uint16_t)), DXGI_FORMAT_R16_UINT };
            }
            return {};
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC10::create()
        {
            if (const std::expected<void, std::string> piped = createPipeline(); !piped)
            {
                return piped;
            }
            if (const std::expected<void, std::string> targeted = createTarget(); !targeted)
            {
                return targeted;
            }
            if (const std::expected<void, std::string> signatures = m_indirect.create(); !signatures)
            {
                return signatures;
            }
            if (const std::expected<void, std::string> recorded = createRecords(); !recorded)
            {
                return recorded;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC10::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Per frame: the list's Reset (a host call), the discard barrier, the pass with the root signature, pipeline,
        // topology, viewport and scissor (the index buffer too when indexed), K ExecuteIndirect, the pass's end and Close.
        std::expected<FrameResult, std::string> NativeC10::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(kCountedTargetEdge), static_cast<float>(kCountedTargetEdge), 0.0f, 1.0f };
            const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(kCountedTargetEdge), static_cast<LONG>(kCountedTargetEdge) };
            D3D12_BARRIER_GROUP discard{};
            discard.Type = D3D12_BARRIER_TYPE_TEXTURE;
            discard.NumBarriers = 1;
            discard.pTextureBarriers = &m_discard;

            const uint64_t start = hostTicks();
            const HRESULT reset = list->Reset(slot.allocators[0], nullptr);
            ++calls.host;
            if (FAILED(reset))
            {
                (void)m_context.endFrame();
                return std::unexpected(failedCall("ID3D12GraphicsCommandList::Reset", reset));
            }
            m_context.startTimestamps(list);
            list->Barrier(1, &discard);
            ++calls.record;
            ++calls.barriers;
            list->BeginRenderPass(1, &m_color, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
            list->SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
            ++calls.record;
            list->SetGraphicsRootSignature(m_context.rootSignature());
            list->SetPipelineState(m_pipeline);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            calls.record += 6;
            if (m_indexed)
            {
                list->IASetIndexBuffer(&m_indices);
                ++calls.record;
            }
            for (uint32_t draw = 0; draw < m_k; ++draw)
            {
                m_indirect.execute(*list, m_draw);
            }
            list->EndRenderPass();
            m_context.endTimestamps(list);
            const HRESULT closed = list->Close();
            calls.record += 2;
            const uint64_t end = hostTicks();
            if (FAILED(closed))
            {
                (void)m_context.endFrame();
                return std::unexpected(failedCall("ID3D12GraphicsCommandList::Close", closed));
            }

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
        // Outside every measurement: the target's tight rows, read after the last frame completed; it stays in
        // LAYOUT_RENDER_TARGET.
        std::expected<std::vector<Output>, std::string> NativeC10::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readTexture(m_target, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "target", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC10(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC10>(config, options);
    }
} // namespace rojoRHI::bench
