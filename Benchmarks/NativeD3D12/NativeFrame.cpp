//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrame.cpp
/// @brief Implements the composed frame's native arm on D3D12: its setup, its five command lists, their submissions and outputs.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeFrame.h"

#include "Common/FrameArms.h"
#include "Common/Runner.h"

#include <cstring>
#include <format>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        // Passes by index in kPasses.
        constexpr uint32_t kCullPass = 0;
        constexpr uint32_t kGeometryPass = 1;
        constexpr uint32_t kAoPass = 2;
        constexpr uint32_t kShadowPass = 3; ///< shadow0; cascade c is pass kShadowPass + c.
        constexpr uint32_t kLightingPass = 6;
        constexpr uint32_t kForwardPass = 7;
        constexpr uint32_t kBloomDown0Pass = 8;
        constexpr uint32_t kBloomDown1Pass = 9;
        constexpr uint32_t kBloomUp1Pass = 10;
        constexpr uint32_t kBloomUp0Pass = 11;
        constexpr uint32_t kToneMapPass = 12;
        constexpr uint32_t kReadbackPass = 13;

        // The global root signature's parameters: the root block's root SRV at t0.
        constexpr UINT kRootBlock = 0;
        // The legacy Unicode form of a PIX event's metadata: a NUL-terminated UTF-16 string.
        constexpr UINT kUnicodeEvent = 0;

        static_assert(kReadbackBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0, "row 540 of ldr is copied at its tight pitch");

        //==============================================================================================================
        constexpr uint64_t arenaBlock(uint64_t bytes)
        {
            return (bytes + NativeContext::kArenaAlignment - 1) / NativeContext::kArenaAlignment * NativeContext::kArenaAlignment;
        }

        // Every arena copy of a frame, each at a 256-byte boundary: the roots of passes 1 to 7, one root per forward box and
        // the five post-processing roots. The camera goes into camera[s], whose raw view the forward roots name.
        constexpr uint64_t kArenaBytes = arenaBlock(sizeof(CullRoot)) + arenaBlock(sizeof(GeometryRoot)) + arenaBlock(sizeof(AoRoot)) +
                                         arenaBlock(sizeof(BlurRoot)) + kCascadeCount * arenaBlock(sizeof(ShadowRoot)) + arenaBlock(sizeof(LightingRoot)) +
                                         kForwardDraws * arenaBlock(sizeof(ForwardRoot)) + 2 * arenaBlock(sizeof(BloomDownRoot)) +
                                         2 * arenaBlock(sizeof(BloomUpRoot)) + arenaBlock(sizeof(ToneMapRoot));

        // Two timestamps per pass: pass p writes queries 2p and 2p + 1 of its slot's heap.
        constexpr uint32_t kTimestamps = 2 * kPassCount;

        //==============================================================================================================
        // The first pass of a list; a list's passes are consecutive in kPasses.
        constexpr uint32_t firstPass(FrameList list)
        {
            for (uint32_t pass = 0; pass < kPassCount; ++pass)
            {
                if (kPasses[pass].list == list)
                {
                    return pass;
                }
            }
            return kPassCount;
        }

        //==============================================================================================================
        constexpr uint32_t passCount(FrameList list)
        {
            uint32_t count = 0;
            for (const PassSpec& pass : kPasses)
            {
                count += pass.list == list ? 1 : 0;
            }
            return count;
        }

        //==============================================================================================================
        template<class T> std::span<const std::byte> bytesOf(const T& value)
        {
            return std::as_bytes(std::span(&value, 1));
        }

        //==============================================================================================================
        // The Addr `offset` bytes past `base`, in the same buffer.
        Addr atOffset(Addr base, uint64_t offset)
        {
            return Addr{ base.lo, base.hi + static_cast<uint32_t>(offset) };
        }

        //==============================================================================================================
        void setRows(const Transform& transform, Float4& row0, Float4& row1, Float4& row2, Float4& row3)
        {
            row0 = transform.row0;
            row1 = transform.row1;
            row2 = transform.row2;
            row3 = transform.row3;
        }

        //==============================================================================================================
        // The labels are ASCII, so each byte is one UTF-16 unit.
        std::wstring wide(std::string_view text)
        {
            std::wstring result;
            result.reserve(text.size());
            for (const char unit : text)
            {
                result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(unit)));
            }
            return result;
        }

        //==============================================================================================================
        bool sameRect(const D3D12_RECT& left, const D3D12_RECT& right)
        {
            return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
        }

        //==============================================================================================================
        std::string failure(std::string_view what, HRESULT result)
        {
            return std::format("frame arm N: {} failed with HRESULT 0x{:08X}", what, static_cast<uint32_t>(result));
        }
    } // namespace

    //==================================================================================================================
    NativeFrame::NativeFrame(NativeContext& context, const Config& /*config*/) : m_context(context), m_resources(context), m_ready(create())
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrame::create()
    {
        if (const std::expected<void, std::string> resources = m_resources.create(); !resources)
        {
            return resources;
        }
        if (const std::expected<void, std::string> lists = createLists(); !lists)
        {
            return lists;
        }
        if (const std::expected<void, std::string> fences = createFences(); !fences)
        {
            return fences;
        }
        createBarriers();
        createRoots();
        // The context's three direct lists of a slot are the gbuffer, shadows and lighting lists, and its arena holds the
        // frame's roots.
        return m_context.prepareFrames(NativeFrameShape{ .lists = kDirectLists, .arenaBytes = kArenaBytes });
    }

    //==================================================================================================================
    // Per slot: the cull and ao lists on compute allocators of their own, a query heap with a timestamp pair per pass and
    // the READBACK buffer the lists resolve it into.
    std::expected<void, std::string> NativeFrame::createLists()
    {
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            SlotLists& lists = m_slots[slot];
            const std::expected<NativeList, std::string> cull =
                m_context.commandList(D3D12_COMMAND_LIST_TYPE_COMPUTE, std::format("bench.frame.cull.{}", slot));
            if (!cull)
            {
                return std::unexpected(cull.error());
            }
            lists.cull = *cull;
            const std::expected<NativeList, std::string> ao = m_context.commandList(D3D12_COMMAND_LIST_TYPE_COMPUTE, std::format("bench.frame.ao.{}", slot));
            if (!ao)
            {
                return std::unexpected(ao.error());
            }
            lists.ao = *ao;
            const std::expected<ID3D12QueryHeap*, std::string> heap = m_context.timestampHeap(kTimestamps, std::format("bench.frame.timestamps.{}", slot));
            if (!heap)
            {
                return std::unexpected(heap.error());
            }
            lists.timestamps = *heap;
            for (const QueueType queue : { QueueType::Graphics, QueueType::Compute })
            {
                const std::string_view name = queue == QueueType::Graphics ? "graphics" : "compute";
                const std::expected<NativeBuffer, std::string> resolved =
                    m_context.readbackBuffer(uint64_t{ kTimestamps } * sizeof(uint64_t), std::format("bench.frame.resolved.{}.{}", slot, name));
                if (!resolved)
                {
                    return std::unexpected(resolved.error());
                }
                lists.resolved[queue == QueueType::Graphics ? 0 : 1] = resolved->resource;
            }
        }
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrame::createFences()
    {
        const std::array<std::pair<ID3D12Fence**, std::string_view>, 3> fences{ {
            { &m_cullDone, "bench.frame.cullDone" },
            { &m_gbufferDone, "bench.frame.gbufferDone" },
            { &m_aoDone, "bench.frame.aoDone" },
        } };
        for (const auto& [fence, label] : fences)
        {
            const std::expected<ID3D12Fence*, std::string> created = m_context.fence(label);
            if (!created)
            {
                return std::unexpected(created.error());
            }
            *fence = *created;
        }
        return {};
    }

    //==================================================================================================================
    // Every batch's texture barriers over the frame's textures, fixed for the arm's life.
    void NativeFrame::createBarriers()
    {
        for (uint32_t index = 0; index < kFrameBatchCount; ++index)
        {
            const NativeBatch& batch = kFrameBarriers[index];
            for (uint32_t row = 0; row < batch.count; ++row)
            {
                const NativeTextureBarrier& lowered = batch.textures[row];
                m_barriers[index][row] = textureBarrier(lowered, m_resources.texture(lowered.texture), textureSpec(lowered.texture).layers);
            }
        }
    }

    //==================================================================================================================
    // Every root the frame pushes, with the context's views, the frame's sampler and the module's Addr of each buffer:
    // only each forward root's camera is filled per frame, with the slot's camera[s].
    void NativeFrame::createRoots()
    {
        for (uint32_t pass = 0; pass < kPassCount; ++pass)
        {
            m_passLabels[pass] = wide(kPasses[pass].label);
        }

        const Addr instances = m_resources.instances();
        const Addr vertices = m_resources.vertices();
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            const Addr records = m_resources.recordsAddr(slot);
            m_cullRoots[slot] = CullRoot{
                .instances = instances,
                .records0 = atOffset(records, viewRecordsOffset(0)),
                .records1 = atOffset(records, viewRecordsOffset(1)),
                .records2 = atOffset(records, viewRecordsOffset(2)),
                .records3 = atOffset(records, viewRecordsOffset(3)),
                .counts0 = atOffset(records, viewCountOffset(0)),
                .counts1 = atOffset(records, viewCountOffset(1)),
                .counts2 = atOffset(records, viewCountOffset(2)),
                .counts3 = atOffset(records, viewCountOffset(3)),
                .pad0 = 0,
                .pad1 = 0,
            };
        }

        m_camera = cameraViewProjection();
        m_geometryRoot = GeometryRoot{ .instances = instances, .vertices = vertices };
        setRows(m_camera, m_geometryRoot.viewProj0, m_geometryRoot.viewProj1, m_geometryRoot.viewProj2, m_geometryRoot.viewProj3);
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            ShadowRoot& shadow = m_shadowRoots[cascade];
            shadow = ShadowRoot{ .instances = instances, .vertices = vertices };
            setRows(cascadeViewProjection(cascade), shadow.viewProj0, shadow.viewProj1, shadow.viewProj2, shadow.viewProj3);
        }

        m_aoRoot = AoRoot{ .depth = m_resources.view(FrameTexture::Depth),
                           .normal = m_resources.view(FrameTexture::Normal),
                           .aoRaw = m_resources.storageView(FrameTexture::AoRaw),
                           .width = kFrameWidth,
                           .height = kFrameHeight,
                           .pad0 = 0,
                           .pad1 = 0,
                           .pad2 = 0 };
        m_blurRoot = BlurRoot{
            .aoRaw = m_resources.view(FrameTexture::AoRaw), .ao = m_resources.storageView(FrameTexture::Ao), .width = kFrameWidth, .height = kFrameHeight
        };

        m_lightingRoot = LightingRoot{ .albedo = m_resources.view(FrameTexture::Albedo),
                                       .normal = m_resources.view(FrameTexture::Normal),
                                       .material = m_resources.view(FrameTexture::Material),
                                       .depth = m_resources.view(FrameTexture::Depth),
                                       .shadow = m_resources.view(FrameTexture::Shadow),
                                       .ao = m_resources.view(FrameTexture::Ao),
                                       .hdr = m_resources.storageView(FrameTexture::Hdr),
                                       .width = kFrameWidth,
                                       .height = kFrameHeight,
                                       .lightDirection = lightDirection() };
        LightingRoot& lighting = m_lightingRoot;
        setRows(cameraInverseViewProjection(), lighting.inverseViewProj0, lighting.inverseViewProj1, lighting.inverseViewProj2, lighting.inverseViewProj3);
        setRows(cascadeViewProjection(0), lighting.cascade0Row0, lighting.cascade0Row1, lighting.cascade0Row2, lighting.cascade0Row3);
        setRows(cascadeViewProjection(1), lighting.cascade1Row0, lighting.cascade1Row1, lighting.cascade1Row2, lighting.cascade1Row3);
        setRows(cascadeViewProjection(2), lighting.cascade2Row0, lighting.cascade2Row1, lighting.cascade2Row2, lighting.cascade2Row3);

        // Each box's vertices start at its mesh's first vertex, since the vertex index excludes the base vertex.
        m_forwardRoots.reserve(kForwardDraws);
        m_forwardMeshes.reserve(kForwardDraws);
        for (uint32_t index = 0; index < kForwardDraws; ++index)
        {
            const ForwardBox box = forwardBox(index);
            const uint32_t mesh = box.box % kMeshCount;
            ForwardRoot root{ .color = box.color, .vertices = atOffset(vertices, uint64_t{ kMeshVertexCount } * mesh * sizeof(MeshVertex)) };
            setRows(box.transform, root.transform0, root.transform1, root.transform2, root.transform3);
            m_forwardRoots.push_back(root);
            m_forwardMeshes.push_back(mesh);
        }

        const uint32_t samplerId = m_resources.sampler();
        m_bloomDown0Root =
            BloomDownRoot{ .source = m_resources.view(FrameTexture::Hdr), .samplerId = samplerId, .texelX = kBloomDown0TexelX, .texelY = kBloomDown0TexelY };
        m_bloomDown1Root =
            BloomDownRoot{ .source = m_resources.view(FrameTexture::Bloom0), .samplerId = samplerId, .texelX = kBloomDown1TexelX, .texelY = kBloomDown1TexelY };
        m_bloomUp1Root = BloomUpRoot{ .source = m_resources.view(FrameTexture::Bloom1), .samplerId = samplerId, .intensity = kBloomUpIntensity, .pad0 = 0 };
        m_bloomUp0Root = BloomUpRoot{ .source = m_resources.view(FrameTexture::Bloom0), .samplerId = samplerId, .intensity = kBloomUpIntensity, .pad0 = 0 };
        m_toneMapRoot = ToneMapRoot{ .source = m_resources.view(FrameTexture::Hdr), .samplerId = samplerId, .exposure = kToneMapExposure, .pad0 = 0 };
    }

    //==================================================================================================================
    const std::expected<void, std::string>& NativeFrame::ready() const
    {
        return m_ready;
    }

    //==================================================================================================================
    void NativeFrame::barrier(ID3D12GraphicsCommandList9& list, FrameBatch batch)
    {
        const size_t index = static_cast<size_t>(batch);
        emitBarriers(list, std::span(m_barriers[index].data(), nativeBatch(batch).count), m_context.calls());
    }

    //==================================================================================================================
    // A pass's opening event and timestamp.
    void NativeFrame::beginPass(ID3D12GraphicsCommandList9& list, ID3D12QueryHeap* heap, uint32_t pass)
    {
        const std::wstring& label = m_passLabels[pass];
        list.BeginEvent(kUnicodeEvent, label.c_str(), static_cast<UINT>((label.size() + 1) * sizeof(wchar_t)));
        list.EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * pass);
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    // A pass's closing timestamp and event.
    void NativeFrame::endPass(ID3D12GraphicsCommandList9& list, ID3D12QueryHeap* heap, uint32_t pass)
    {
        list.EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * pass + 1);
        list.EndEvent();
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    void NativeFrame::openRenderPass(ID3D12GraphicsCommandList9& list, uint32_t pass)
    {
        const NativeFramePass& target = m_resources.pass(pass);
        list.BeginRenderPass(
            target.colorCount, target.colorCount > 0 ? target.colors.data() : nullptr, target.hasDepth ? &target.depth : nullptr, D3D12_RENDER_PASS_FLAG_NONE
        );
        ++m_context.calls().record;
    }

    //==================================================================================================================
    // The heaps before the root signature, which indexes them directly.
    void NativeFrame::bindCompute(ID3D12GraphicsCommandList9& list)
    {
        list.SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
        list.SetComputeRootSignature(m_context.rootSignature());
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    // The heaps, the root signature, and the triangle topology and UInt16 index buffer every draw of the frame uses.
    void NativeFrame::bindGraphics(ID3D12GraphicsCommandList9& list)
    {
        list.SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
        list.SetGraphicsRootSignature(m_context.rootSignature());
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.IASetIndexBuffer(&m_resources.indices());
        m_context.calls().record += 4;
    }

    //==================================================================================================================
    // A list's end: the submission timestamps' last, then one ResolveQueryData of its passes' timestamps into the slot's
    // READBACK buffer of the list's queue, then Close.
    HRESULT NativeFrame::finish(ID3D12GraphicsCommandList9& list, const SlotLists& lists, FrameList frameList)
    {
        m_context.endTimestamps(&list);
        const UINT first = 2 * firstPass(frameList);
        ID3D12Resource* resolved = lists.resolved[listSpec(frameList).queue == QueueType::Graphics ? 0 : 1];
        list.ResolveQueryData(lists.timestamps, D3D12_QUERY_TYPE_TIMESTAMP, first, 2 * passCount(frameList), resolved, uint64_t{ first } * sizeof(uint64_t));
        const HRESULT closed = list.Close();
        m_context.calls().record += 2;
        return closed;
    }

    //==================================================================================================================
    // Pass 1: the culling dispatch, one thread per box and view.
    HRESULT NativeFrame::recordCull(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex)
    {
        NativeCalls& calls = m_context.calls();
        bindCompute(list);
        beginPass(list, lists.timestamps, kCullPass);
        list.SetPipelineState(m_resources.pipelines().cull);
        list.SetComputeRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_cullRoots[slotIndex])));
        list.Dispatch(kCullGroups.width, kCullGroups.height, kCullGroups.depth);
        calls.record += 3;
        endPass(list, lists.timestamps, kCullPass);
        return finish(list, lists, FrameList::Cull);
    }

    //==================================================================================================================
    // Pass 2: the camera's counted draw into the G-buffer, then the batch that hands it to the kernels.
    HRESULT NativeFrame::recordGbuffer(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex)
    {
        NativeCalls& calls = m_context.calls();
        const NativeFramePass& geometry = m_resources.pass(kGeometryPass);
        ID3D12Resource* records = m_resources.records(slotIndex);
        bindGraphics(list);
        barrier(list, FrameBatch::Geometry);
        beginPass(list, lists.timestamps, kGeometryPass);
        openRenderPass(list, kGeometryPass);
        list.SetPipelineState(m_resources.pipelines().geometry);
        list.RSSetViewports(1, &geometry.viewport);
        list.RSSetScissorRects(1, &geometry.scissor);
        list.SetGraphicsRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_geometryRoot)));
        calls.record += 4;
        m_resources.indirect().execute(
            list,
            CountedDraw{ .records = records,
                         .recordsOffset = viewRecordsOffset(kCameraView),
                         .count = records,
                         .countOffset = viewCountOffset(kCameraView),
                         .maxCount = kRecordsPerView,
                         .indexed = true }
        );
        list.EndRenderPass();
        ++calls.record;
        endPass(list, lists.timestamps, kGeometryPass);
        barrier(list, FrameBatch::GbufferClose);
        return finish(list, lists, FrameList::Gbuffer);
    }

    //==================================================================================================================
    // Pass 3: the occlusion dispatch, the barrier on aoRaw, then the blur, and the batch that hands `ao` to the lighting.
    HRESULT NativeFrame::recordAo(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists)
    {
        NativeCalls& calls = m_context.calls();
        const NativeFramePipelines& pipelines = m_resources.pipelines();
        bindCompute(list);
        barrier(list, FrameBatch::AoStart);
        beginPass(list, lists.timestamps, kAoPass);
        list.SetPipelineState(pipelines.ao);
        list.SetComputeRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_aoRoot)));
        list.Dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        calls.record += 3;
        barrier(list, FrameBatch::AoRaw);
        list.SetPipelineState(pipelines.blur);
        list.SetComputeRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_blurRoot)));
        list.Dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        calls.record += 3;
        endPass(list, lists.timestamps, kAoPass);
        barrier(list, FrameBatch::AoClose);
        return finish(list, lists, FrameList::Ao);
    }

    //==================================================================================================================
    // Passes 4 to 6: each cascade's counted draw into its layer of `shadow`. The pipeline, viewport and scissor of the
    // three passes are the same, so they are set once.
    HRESULT NativeFrame::recordShadows(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex)
    {
        NativeCalls& calls = m_context.calls();
        const NativeFramePass& cascades = m_resources.pass(kShadowPass);
        ID3D12Resource* records = m_resources.records(slotIndex);
        bindGraphics(list);
        list.SetPipelineState(m_resources.pipelines().shadow);
        list.RSSetViewports(1, &cascades.viewport);
        list.RSSetScissorRects(1, &cascades.scissor);
        calls.record += 3;
        barrier(list, FrameBatch::Shadows);
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            const uint32_t pass = kShadowPass + cascade;
            beginPass(list, lists.timestamps, pass);
            openRenderPass(list, pass);
            list.SetGraphicsRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_shadowRoots[cascade])));
            ++calls.record;
            m_resources.indirect().execute(
                list,
                CountedDraw{ .records = records,
                             .recordsOffset = viewRecordsOffset(cascade),
                             .count = records,
                             .countOffset = viewCountOffset(cascade),
                             .maxCount = kRecordsPerView,
                             .indexed = true }
            );
            list.EndRenderPass();
            ++calls.record;
            endPass(list, lists.timestamps, pass);
        }
        return finish(list, lists, FrameList::Shadows);
    }

    //==================================================================================================================
    // Passes 7 to 14: lighting, the camera upload, the forward boxes, bloom, tone map and the copy of row 540.
    HRESULT NativeFrame::recordLighting(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex)
    {
        NativeCalls& calls = m_context.calls();
        const NativeFramePipelines& pipelines = m_resources.pipelines();
        ID3D12QueryHeap* heap = lists.timestamps;
        list.SetDescriptorHeaps(static_cast<UINT>(m_context.heaps().size()), m_context.heaps().data());
        list.SetComputeRootSignature(m_context.rootSignature());
        list.SetGraphicsRootSignature(m_context.rootSignature());
        list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.IASetIndexBuffer(&m_resources.indices());
        calls.record += 5;

        // 7 lighting.
        barrier(list, FrameBatch::Lighting);
        beginPass(list, heap, kLightingPass);
        list.SetPipelineState(pipelines.lighting);
        list.SetComputeRootShaderResourceView(kRootBlock, slot.push(bytesOf(m_lightingRoot)));
        list.Dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        calls.record += 3;
        endPass(list, heap, kLightingPass);

        // The camera's view-projection, read by every forward box: one copy into the slot's mapped camera[s], which the
        // slot's previous frame has finished reading.
        std::memcpy(m_resources.cameraData(slotIndex), &m_camera, sizeof(m_camera));
        const Addr cameraAddress = m_resources.camera(slotIndex);

        // 8 forward: 500 boxes, each a root and a direct indexed draw of its mesh.
        const NativeFramePass& forward = m_resources.pass(kForwardPass);
        barrier(list, FrameBatch::Forward);
        beginPass(list, heap, kForwardPass);
        openRenderPass(list, kForwardPass);
        list.SetPipelineState(pipelines.forward);
        list.RSSetViewports(1, &forward.viewport);
        list.RSSetScissorRects(1, &forward.scissor);
        calls.record += 3;
        for (uint32_t box = 0; box < kForwardDraws; ++box)
        {
            ForwardRoot root = m_forwardRoots[box];
            root.camera = cameraAddress;
            const uint32_t mesh = m_forwardMeshes[box];
            list.SetGraphicsRootShaderResourceView(kRootBlock, slot.push(bytesOf(root)));
            list.DrawIndexedInstanced(kMeshIndexCount, 1, kMeshIndexCount * mesh, static_cast<INT>(kMeshVertexCount * mesh), 0);
            calls.record += 2;
        }
        list.EndRenderPass();
        ++calls.record;
        endPass(list, heap, kForwardPass);

        // 9 to 13: bloom down twice, bloom up twice and the tone map, each one full-screen triangle.
        struct Fullscreen
        {
            uint32_t pass;
            FrameBatch batch;
            ID3D12PipelineState* pipeline;
            std::span<const std::byte> root;
        };
        const std::array<Fullscreen, 5> post{ {
            { kBloomDown0Pass, FrameBatch::BloomDown0, pipelines.bloomDown, bytesOf(m_bloomDown0Root) },
            { kBloomDown1Pass, FrameBatch::BloomDown1, pipelines.bloomDown, bytesOf(m_bloomDown1Root) },
            { kBloomUp1Pass, FrameBatch::BloomUp1, pipelines.bloomUp, bytesOf(m_bloomUp1Root) },
            { kBloomUp0Pass, FrameBatch::BloomUp0, pipelines.bloomUp, bytesOf(m_bloomUp0Root) },
            { kToneMapPass, FrameBatch::ToneMap, pipelines.toneMap, bytesOf(m_toneMapRoot) },
        } };
        ID3D12PipelineState* bound = pipelines.forward;
        D3D12_RECT covered = forward.scissor;
        for (const Fullscreen& fullscreen : post)
        {
            const NativeFramePass& target = m_resources.pass(fullscreen.pass);
            barrier(list, fullscreen.batch);
            beginPass(list, heap, fullscreen.pass);
            openRenderPass(list, fullscreen.pass);
            if (fullscreen.pipeline != bound)
            {
                list.SetPipelineState(fullscreen.pipeline);
                ++calls.record;
                bound = fullscreen.pipeline;
            }
            if (!sameRect(target.scissor, covered))
            {
                list.RSSetViewports(1, &target.viewport);
                list.RSSetScissorRects(1, &target.scissor);
                calls.record += 2;
                covered = target.scissor;
            }
            list.SetGraphicsRootShaderResourceView(kRootBlock, slot.push(fullscreen.root));
            list.DrawInstanced(3, 1, 0, 0);
            list.EndRenderPass();
            calls.record += 3;
            endPass(list, heap, fullscreen.pass);
        }

        // 14 readback: row 540 of ldr into readback[s] at its tight pitch.
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_resources.readback(slotIndex);
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = 0;
        destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        destination.PlacedFootprint.Footprint.Width = kFrameWidth;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackBytes);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = m_resources.texture(FrameTexture::Ldr);
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        const D3D12_BOX row{ 0, kReadbackRow, 0, kFrameWidth, kReadbackRow + 1, 1 };
        barrier(list, FrameBatch::Readback);
        beginPass(list, heap, kReadbackPass);
        list.CopyTextureRegion(&destination, 0, 0, 0, &source, &row);
        ++calls.record;
        endPass(list, heap, kReadbackPass);
        return finish(list, lists, FrameList::Lighting);
    }

    //==================================================================================================================
    // The five submissions in order. A queue's Wait holds every later submission of that queue, and its Signal follows the
    // submissions before it, so the shadows overlap the occlusion on the compute queue; the lighting list's wait on aoDone
    // joins the compute queue's work before the frame fence's signal.
    HRESULT NativeFrame::submit(const SlotLists& lists, const NativeContext::Slot& slot, uint64_t value)
    {
        NativeCalls& calls = m_context.calls();
        ID3D12CommandQueue* graphics = m_context.graphicsQueue();
        ID3D12CommandQueue* compute = m_context.computeQueue();
        ID3D12CommandList* const cull[] = { lists.cull.list };
        ID3D12CommandList* const gbuffer[] = { slot.lists[kGbufferList] };
        ID3D12CommandList* const ao[] = { lists.ao.list };
        ID3D12CommandList* const shadows[] = { slot.lists[kShadowsList] };
        ID3D12CommandList* const lighting[] = { slot.lists[kLightingList] };
        HRESULT first = S_OK;
        const auto keep = [&first](HRESULT result) {
            if (FAILED(result) && SUCCEEDED(first))
            {
                first = result;
            }
        };

        m_context.commit(compute, cull);
        keep(compute->Signal(m_cullDone, value));
        keep(graphics->Wait(m_cullDone, value));
        m_context.commit(graphics, gbuffer);
        keep(graphics->Signal(m_gbufferDone, value));
        keep(compute->Wait(m_gbufferDone, value));
        m_context.commit(compute, ao);
        keep(compute->Signal(m_aoDone, value));
        m_context.commit(graphics, shadows);
        keep(graphics->Wait(m_aoDone, value));
        m_context.commit(graphics, lighting);
        calls.submit += 6;
        return first;
    }

    //==================================================================================================================
    // Every list is recorded before the first submission. The context resets the allocators of its three direct lists;
    // the frame resets those of its two compute lists and all five lists, each a counted host call, unless the slot's
    // wait failed, when the frame records nothing.
    std::expected<FrameResult, std::string> NativeFrame::frame(uint32_t index)
    {
        const uint32_t slotIndex = slotOf(index);
        NativeContext::Slot& slot = m_context.beginFrame(index, kListCount);
        // A failed slot wait leaves the slot's lists and allocators to the GPU, as the context leaves its own.
        if (m_context.frameFailed())
        {
            const std::expected<std::optional<double>, std::string> ended = m_context.endFrame();
            return std::unexpected(ended ? std::string("frame arm N: the frame slot could not be reused") : ended.error());
        }
        SlotLists& lists = m_slots[slotIndex];
        NativeCalls& calls = m_context.calls();
        ID3D12GraphicsCommandList9& gbuffer = *slot.lists[kGbufferList];
        ID3D12GraphicsCommandList9& shadows = *slot.lists[kShadowsList];
        ID3D12GraphicsCommandList9& lighting = *slot.lists[kLightingList];

        HRESULT reset = S_OK;
        for (ID3D12CommandAllocator* allocator : { lists.cull.allocator, lists.ao.allocator })
        {
            const HRESULT cleared = allocator->Reset();
            ++calls.host;
            reset = FAILED(reset) ? reset : cleared;
        }
        const std::array<std::pair<ID3D12GraphicsCommandList9*, ID3D12CommandAllocator*>, kListCount> resets{ {
            { lists.cull.list, lists.cull.allocator },
            { &gbuffer, slot.allocators[kGbufferList] },
            { lists.ao.list, lists.ao.allocator },
            { &shadows, slot.allocators[kShadowsList] },
            { &lighting, slot.allocators[kLightingList] },
        } };
        for (const auto& [list, allocator] : resets)
        {
            const HRESULT cleared = list->Reset(allocator, nullptr);
            ++calls.host;
            reset = FAILED(reset) ? reset : cleared;
            if (SUCCEEDED(cleared))
            {
                const bool onCompute = list == lists.cull.list || list == lists.ao.list;
                m_context.startTimestamps(list, onCompute ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT);
            }
        }
        if (FAILED(reset))
        {
            (void)m_context.endFrame();
            return std::unexpected(failure("Reset of the frame's allocators and lists", reset));
        }

        HRESULT closed = recordCull(*lists.cull.list, slot, lists, slotIndex);
        const HRESULT gbufferClosed = recordGbuffer(gbuffer, slot, lists, slotIndex);
        const HRESULT aoClosed = recordAo(*lists.ao.list, slot, lists);
        const HRESULT shadowsClosed = recordShadows(shadows, slot, lists, slotIndex);
        const HRESULT lightingClosed = recordLighting(lighting, slot, lists, slotIndex);
        for (const HRESULT result : { gbufferClosed, aoClosed, shadowsClosed, lightingClosed })
        {
            closed = FAILED(closed) ? closed : result;
        }
        if (FAILED(closed))
        {
            (void)m_context.endFrame();
            return std::unexpected(failure("Close of a frame list", closed));
        }

        const HRESULT submitted = submit(lists, slot, uint64_t{ index } + 1);
        const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
        if (FAILED(submitted))
        {
            return std::unexpected(failure("a fence Signal or Wait of the frame's submissions", submitted));
        }
        if (!gpu)
        {
            return std::unexpected(gpu.error());
        }
        m_lastIndex = index;
        return FrameResult{ .cpuNs = std::nullopt, .gpuNs = *gpu };
    }

    //==================================================================================================================
    std::expected<std::vector<Output>, std::string> NativeFrame::readback()
    {
        if (!m_lastIndex)
        {
            return std::unexpected(std::string("frame outputs: no frame has run"));
        }
        return m_resources.readOutputs(slotOf(*m_lastIndex));
    }

    //==================================================================================================================
    ArmResult runNativeFrame(const Config& config, const RunOptions& options)
    {
        return runArm<NativeFrame>(config, options);
    }
} // namespace rojoRHI::bench
