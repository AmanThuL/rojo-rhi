//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrame.cpp
/// @brief Implements the composed frame's native arm: its setup, its five command buffers, their submissions and its outputs.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeFrame.h"

#include "Common/FrameArms.h"
#include "Common/FrameRoots.h"
#include "Common/Runner.h"
#include "Common/Scene.h"
#include "Native/NativeFrameBarriers.h"

#include <format>
#include <span>

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

        // Counted draws, in the order NativeIndirect holds them: the camera's, then cascades 0 to 2.
        constexpr uint32_t kGeometryDraw = 0;
        constexpr uint32_t kShadowDraw = 1; ///< Cascade c is draw kShadowDraw + c.
        constexpr uint32_t kCountedDraws = 1 + kCascadeCount;
        constexpr uint32_t kCountedMaxCount = kRecordsPerView;

        //==============================================================================================================
        constexpr uint64_t arenaBlock(uint64_t bytes)
        {
            return (bytes + NativeContext::kArenaAlignment - 1) / NativeContext::kArenaAlignment * NativeContext::kArenaAlignment;
        }

        // Every arena copy of a frame, each at a 256-byte boundary: the roots of passes 1 to 7, the camera upload, one
        // root per forward box and the five post-processing roots.
        constexpr uint64_t kArenaBytes = arenaBlock(sizeof(CullRoot)) + arenaBlock(sizeof(GeometryRoot)) + arenaBlock(sizeof(AoRoot)) +
                                         arenaBlock(sizeof(BlurRoot)) + kCascadeCount * arenaBlock(sizeof(ShadowRoot)) + arenaBlock(sizeof(LightingRoot)) +
                                         arenaBlock(kCameraUploadBytes) + kForwardDraws * arenaBlock(sizeof(ForwardRoot)) +
                                         2 * arenaBlock(sizeof(BloomDownRoot)) + 2 * arenaBlock(sizeof(BloomUpRoot)) + arenaBlock(sizeof(ToneMapRoot));

        //==============================================================================================================
        // Timestamps per list: a pair per pass.
        constexpr std::array<uint32_t, kListCount> timestampCounts()
        {
            std::array<uint32_t, kListCount> counts{};
            for (const PassSpec& pass : kPasses)
            {
                counts[static_cast<size_t>(pass.list)] += 2;
            }
            return counts;
        }

        constexpr std::array<uint32_t, kListCount> kTimestamps = timestampCounts();

        //==============================================================================================================
        // The index in kRenderPasses of the render pass that is pass `pass` of kPasses.
        constexpr uint32_t renderPassIndex(uint32_t pass)
        {
            for (uint32_t index = 0; index < kRenderPassCount; ++index)
            {
                if (kRenderPasses[index].pass == pass)
                {
                    return index;
                }
            }
            return kRenderPassCount;
        }

        //==============================================================================================================
        MTL::BlendFactor blendFactor(BlendFactor factor)
        {
            switch (factor)
            {
            case BlendFactor::Zero: return MTL::BlendFactorZero;
            case BlendFactor::One: return MTL::BlendFactorOne;
            case BlendFactor::SrcAlpha: return MTL::BlendFactorSourceAlpha;
            case BlendFactor::OneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
            default: return MTL::BlendFactorZero;
            }
        }

        //==============================================================================================================
        MTL::LoadAction loadAction(LoadOp load)
        {
            switch (load)
            {
            case LoadOp::Load: return MTL::LoadActionLoad;
            case LoadOp::Clear: return MTL::LoadActionClear;
            case LoadOp::DontCare: return MTL::LoadActionDontCare;
            }
            return MTL::LoadActionDontCare;
        }

        //==============================================================================================================
        MTL::PixelFormat colorFormat(FrameTexture texture)
        {
            return textureSpec(texture).format == Format::RGBA16Float ? MTL::PixelFormatRGBA16Float : MTL::PixelFormatRGBA8Unorm;
        }

        //==============================================================================================================
        template<class T> std::span<const std::byte> bytesOf(const T& value)
        {
            return std::as_bytes(std::span(&value, 1));
        }
    } // namespace

    //==================================================================================================================
    NativeFrame::NativeFrame(NativeContext& context, const Config& /*config*/)
        : m_context(context), m_resources(context), m_indirect(context), m_ready(create())
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrame::create()
    {
        if (const std::expected<void, std::string> resources = m_resources.create(); !resources)
        {
            return resources;
        }
        if (const std::expected<void, std::string> pipelines = createPipelines(); !pipelines)
        {
            return pipelines;
        }
        if (const std::expected<void, std::string> passes = createPasses(); !passes)
        {
            return passes;
        }
        if (const std::expected<void, std::string> counted = createCountedDraws(); !counted)
        {
            return counted;
        }
        if (const std::expected<void, std::string> lists = createLists(); !lists)
        {
            return lists;
        }
        if (const std::expected<void, std::string> events = createEvents(); !events)
        {
            return events;
        }
        // The context's one list per slot carries the output dumps; the frame records its own five.
        return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = kArenaBytes });
    }

    //==================================================================================================================
    // Every render pipeline takes triangles; blending is the pass's blend of color attachment 0.
    std::expected<void, std::string> NativeFrame::createPipelines()
    {
        const auto compute = [&](std::string_view library, std::string_view entry, MTL::ComputePipelineState*& out) -> std::expected<void, std::string> {
            const std::expected<MTL::Library*, std::string> loaded = m_context.library(library);
            if (!loaded)
            {
                return std::unexpected(loaded.error());
            }
            const std::expected<MTL::ComputePipelineState*, std::string> pipeline = m_context.computePipeline(*loaded, entry, std::format("frame.{}", entry));
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            out = *pipeline;
            return {};
        };
        const auto render = [&](
                                std::string_view library, std::string_view vertex, std::string_view fragment, uint32_t pass, MTL::RenderPipelineState*& out
                            ) -> std::expected<void, std::string> {
            const std::expected<MTL::Library*, std::string> loaded = m_context.library(library);
            if (!loaded)
            {
                return std::unexpected(loaded.error());
            }
            const RenderPassSpec& spec = kRenderPasses[renderPassIndex(pass)];
            MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
            descriptor->setVertexFunctionDescriptor(m_context.function(*loaded, vertex));
            if (!fragment.empty())
            {
                descriptor->setFragmentFunctionDescriptor(m_context.function(*loaded, fragment));
            }
            descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
            for (uint32_t color = 0; color < spec.colorCount; ++color)
            {
                MTL4::RenderPipelineColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(color);
                attachment->setPixelFormat(colorFormat(spec.colors[color].texture));
                if (color == 0 && spec.blend.enable)
                {
                    attachment->setBlendingState(MTL4::BlendStateEnabled);
                    attachment->setSourceRGBBlendFactor(blendFactor(spec.blend.srcColor));
                    attachment->setDestinationRGBBlendFactor(blendFactor(spec.blend.dstColor));
                    attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
                    attachment->setSourceAlphaBlendFactor(blendFactor(spec.blend.srcAlpha));
                    attachment->setDestinationAlphaBlendFactor(blendFactor(spec.blend.dstAlpha));
                    attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
                }
            }
            const std::expected<MTL::RenderPipelineState*, std::string> pipeline =
                m_context.renderPipeline(descriptor, std::format("frame.{}", kPasses[pass].label));
            descriptor->release();
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            out = *pipeline;
            return {};
        };

        for (const std::expected<void, std::string>& made : {
                 compute("BenchCull", "cullMain", m_pipelines.cull),
                 compute("BenchAo", "aoMain", m_pipelines.ao),
                 compute("BenchAo", "aoBlurMain", m_pipelines.blur),
                 compute("BenchLighting", "lightingMain", m_pipelines.lighting),
                 render("BenchGBuffer", "gbufferVertex", "gbufferFragment", kGeometryPass, m_pipelines.geometry),
                 render("BenchShadow", "shadowVertex", "", kShadowPass, m_pipelines.shadow),
                 render("BenchForward", "forwardVertex", "forwardFragment", kForwardPass, m_pipelines.forward),
                 render("BenchPost", "fullscreenVertex", "bloomDownFragment", kBloomDown0Pass, m_pipelines.bloomDown),
                 render("BenchPost", "fullscreenVertex", "bloomUpFragment", kBloomUp1Pass, m_pipelines.bloomUp),
                 render("BenchPost", "fullscreenVertex", "toneMapFragment", kToneMapPass, m_pipelines.toneMap),
             })
        {
            if (!made)
            {
                return made;
            }
        }

        // kDepthWrite and kDepthTestOnly: Greater, reverse Z, with and without depth writes.
        MTL::DepthStencilDescriptor* depth = MTL::DepthStencilDescriptor::alloc()->init();
        depth->setDepthCompareFunction(MTL::CompareFunctionGreater);
        depth->setDepthWriteEnabled(true);
        depth->setLabel(m_context.string("frame.depthWrite"));
        m_depthWrite = m_context.device()->newDepthStencilState(depth);
        depth->setDepthWriteEnabled(false);
        depth->setLabel(m_context.string("frame.depthTestOnly"));
        m_depthTestOnly = m_context.device()->newDepthStencilState(depth);
        depth->release();
        m_context.calls().host += 2;
        if (m_depthWrite == nullptr || m_depthTestOnly == nullptr)
        {
            return std::unexpected("frame: the device refused a depth-stencil state");
        }
        m_context.own(m_depthWrite);
        m_context.own(m_depthTestOnly);
        return {};
    }

    //==================================================================================================================
    // One render pass descriptor per render pass of kRenderPasses; every attachment stores.
    std::expected<void, std::string> NativeFrame::createPasses()
    {
        for (uint32_t pass = 0; pass < kPassCount; ++pass)
        {
            m_passLabels[pass] = m_context.string(kPasses[pass].label);
        }
        for (uint32_t index = 0; index < kRenderPassCount; ++index)
        {
            const RenderPassSpec& spec = kRenderPasses[index];
            MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
            m_context.own(descriptor);
            for (uint32_t color = 0; color < spec.colorCount; ++color)
            {
                const ColorPassAttachment& source = spec.colors[color];
                MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(color);
                attachment->setTexture(m_resources.texture(source.texture));
                attachment->setLoadAction(loadAction(source.load));
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearColor(MTL::ClearColor::Make(source.clear.r, source.clear.g, source.clear.b, source.clear.a));
            }
            if (spec.hasDepth)
            {
                MTL::RenderPassDepthAttachmentDescriptor* attachment = descriptor->depthAttachment();
                attachment->setTexture(m_resources.texture(spec.depth.texture));
                attachment->setSlice(spec.depth.layer);
                attachment->setLoadAction(loadAction(spec.depth.load));
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearDepth(spec.depth.clear);
            }
            m_renderPasses[index] = descriptor;
        }
        return {};
    }

    //==================================================================================================================
    // The camera's counted draw reads view kCameraView of records[s], cascade c's view c; every one is maxCount 10,000
    // with the count the cull wrote, over the scene's UInt16 indices, drawn as triangles.
    std::expected<void, std::string> NativeFrame::createCountedDraws()
    {
        std::array<CountedDraw, kCountedDraws> draws{};
        for (uint32_t draw = 0; draw < kCountedDraws; ++draw)
        {
            const uint32_t view = draw == kGeometryDraw ? kCameraView : draw - kShadowDraw;
            for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
            {
                draws[draw].records[slot] = m_resources.records(slot)->gpuAddress() + viewRecordsOffset(view);
                draws[draw].counts[slot] = m_resources.records(slot)->gpuAddress() + viewCountOffset(view);
            }
            draws[draw].indices = m_resources.indices()->gpuAddress();
            draws[draw].maxCount = kCountedMaxCount;
            draws[draw].indexed = true;
            draws[draw].triangles = true;
        }
        return m_indirect.create(draws);
    }

    //==================================================================================================================
    // Per slot: an allocator and a command buffer per list, both labelled with the list's label, a timestamp heap per
    // list with a pair per pass, and an argument table holding the view and sampler ID tables at buffers 1 and 2.
    std::expected<void, std::string> NativeFrame::createLists()
    {
        NativeCalls& calls = m_context.calls();
        MTL4::CommandAllocatorDescriptor* allocatorDescriptor = MTL4::CommandAllocatorDescriptor::alloc()->init();
        MTL4::CounterHeapDescriptor* heapDescriptor = MTL4::CounterHeapDescriptor::alloc()->init();
        heapDescriptor->setType(MTL4::CounterHeapTypeTimestamp);
        MTL4::ArgumentTableDescriptor* tableDescriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
        tableDescriptor->setMaxBufferBindCount(NativeContext::kArgumentBuffers);
        const auto release = [&]() {
            allocatorDescriptor->release();
            heapDescriptor->release();
            tableDescriptor->release();
        };

        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            SlotLists& lists = m_slots[slot];
            for (uint32_t list = 0; list < kListCount; ++list)
            {
                NS::String* label = m_context.string(kLists[list].label);
                allocatorDescriptor->setLabel(label);
                NS::Error* error = nullptr;
                lists.allocators[list] = m_context.device()->newCommandAllocator(allocatorDescriptor, &error);
                lists.lists[list] = m_context.device()->newCommandBuffer();
                calls.host += 2;
                if (lists.allocators[list] == nullptr || lists.lists[list] == nullptr)
                {
                    release();
                    return std::unexpected(std::format("frame slot {}: list {}: {}", slot, kLists[list].label, describe(error)));
                }
                m_context.own(lists.allocators[list]);
                m_context.own(lists.lists[list]);
                lists.lists[list]->setLabel(label);
                ++calls.record;

                heapDescriptor->setCount(kTimestamps[list]);
                error = nullptr;
                lists.heaps[list] = m_context.device()->newCounterHeap(heapDescriptor, &error);
                ++calls.host;
                if (lists.heaps[list] == nullptr)
                {
                    release();
                    return std::unexpected(std::format("frame slot {}: timestamp heap of {}: {}", slot, kLists[list].label, describe(error)));
                }
                m_context.own(lists.heaps[list]);
                lists.heaps[list]->setLabel(label);
                ++calls.host;
            }

            tableDescriptor->setLabel(m_context.string(std::format("frame.arguments.{}", slot)));
            NS::Error* error = nullptr;
            lists.table = m_context.device()->newArgumentTable(tableDescriptor, &error);
            ++calls.host;
            if (lists.table == nullptr)
            {
                release();
                return std::unexpected(std::format("frame slot {}: argument table: {}", slot, describe(error)));
            }
            m_context.own(lists.table);
            lists.table->setAddress(m_resources.viewTable(), 1);
            lists.table->setAddress(m_resources.samplerTable(), 2);
            calls.host += 2;
        }
        release();
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrame::createEvents()
    {
        const std::array<std::pair<MTL::SharedEvent**, std::string_view>, 3> events{ {
            { &m_cullDone, "cullDone" },
            { &m_gbufferDone, "gbufferDone" },
            { &m_aoDone, "aoDone" },
        } };
        for (const auto& [event, label] : events)
        {
            *event = m_context.device()->newSharedEvent();
            ++m_context.calls().host;
            if (*event == nullptr)
            {
                return std::unexpected(std::format("frame: event {} could not be created", label));
            }
            m_context.own(*event);
            (*event)->setLabel(m_context.string(label));
            ++m_context.calls().host;
        }
        return {};
    }

    //==================================================================================================================
    const std::expected<void, std::string>& NativeFrame::ready() const
    {
        return m_ready;
    }

    //==================================================================================================================
    // The slot's previous frame completed before this one began, so its timestamps are final.
    void NativeFrame::resolve(SlotLists& lists)
    {
        if (!lists.written)
        {
            return;
        }
        for (uint32_t list = 0; list < kListCount; ++list)
        {
            (void)lists.heaps[list]->resolveCounterRange(NS::Range::Make(0, kTimestamps[list]));
            ++m_context.calls().host;
        }
    }

    //==================================================================================================================
    // A pass's opening debug group and timestamp, on the command buffer.
    void NativeFrame::beginPass(MTL4::CommandBuffer* commands, MTL4::CounterHeap* heap, uint32_t pass, uint32_t entry)
    {
        commands->pushDebugGroup(m_passLabels[pass]);
        commands->writeTimestampIntoHeap(heap, entry);
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    // A pass's closing timestamp and debug group, on the command buffer once its encoder has ended.
    void NativeFrame::endPass(MTL4::CommandBuffer* commands, MTL4::CounterHeap* heap, uint32_t entry)
    {
        commands->writeTimestampIntoHeap(heap, entry);
        commands->popDebugGroup();
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    MTL4::RenderCommandEncoder* NativeFrame::openRenderPass(MTL4::CommandBuffer* commands, uint32_t pass)
    {
        MTL4::RenderCommandEncoder* encoder = commands->renderCommandEncoder(m_renderPasses[renderPassIndex(pass)]);
        encoder->setLabel(m_passLabels[pass]);
        m_context.calls().record += 2;
        return encoder;
    }

    //==================================================================================================================
    // The pipeline, the depth-stencil state of a pass with depth, back-face culling with counter-clockwise front faces
    // where the pass culls (Metal's defaults are no culling and no depth test), the viewport, the scissor and the slot's
    // argument table for the vertex and fragment stages.
    void NativeFrame::setRenderState(MTL4::RenderCommandEncoder* encoder, uint32_t pass, MTL::RenderPipelineState* pipeline, MTL4::ArgumentTable* table)
    {
        const RenderPassSpec& spec = kRenderPasses[renderPassIndex(pass)];
        NativeCalls& calls = m_context.calls();
        encoder->setRenderPipelineState(pipeline);
        ++calls.record;
        if (spec.hasDepth)
        {
            encoder->setDepthStencilState(spec.depthStencil.depthWrite ? m_depthWrite : m_depthTestOnly);
            ++calls.record;
        }
        if (spec.raster.cull == CullMode::Back)
        {
            encoder->setCullMode(MTL::CullModeBack);
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            calls.record += 2;
        }
        const Viewport& viewport = spec.viewport;
        encoder->setViewport(MTL::Viewport{ viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth });
        encoder->setScissorRect(MTL::ScissorRect{ spec.scissor.x, spec.scissor.y, spec.scissor.width, spec.scissor.height });
        encoder->setArgumentTable(table, MTL::RenderStageVertex | MTL::RenderStageFragment);
        calls.record += 3;
    }

    //==================================================================================================================
    void NativeFrame::drawFullscreen(MTL4::RenderCommandEncoder* encoder, MTL4::ArgumentTable* table, uint64_t root)
    {
        table->setAddress(root, 0);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{ 0 }, NS::UInteger{ 3 });
        m_context.calls().record += 2;
    }

    //==================================================================================================================
    // Pass 1: the culling dispatch, one thread per box and view.
    void NativeFrame::recordCull(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex)
    {
        const size_t list = static_cast<size_t>(FrameList::Cull);
        MTL4::CommandBuffer* commands = lists.lists[list];
        MTL4::CounterHeap* heap = lists.heaps[list];
        NativeCalls& calls = m_context.calls();
        const uint64_t records = m_resources.records(slotIndex)->gpuAddress();
        const CullRoot root{
            .instances = nativeAddr(m_resources.instances()->gpuAddress()),
            .records0 = nativeAddr(records + viewRecordsOffset(0)),
            .records1 = nativeAddr(records + viewRecordsOffset(1)),
            .records2 = nativeAddr(records + viewRecordsOffset(2)),
            .records3 = nativeAddr(records + viewRecordsOffset(3)),
            .counts0 = nativeAddr(records + viewCountOffset(0)),
            .counts1 = nativeAddr(records + viewCountOffset(1)),
            .counts2 = nativeAddr(records + viewCountOffset(2)),
            .counts3 = nativeAddr(records + viewCountOffset(3)),
            .pad0 = 0,
            .pad1 = 0,
        };

        commands->beginCommandBuffer(lists.allocators[list]);
        ++calls.record;
        beginPass(commands, heap, kCullPass, 0);
        MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
        encoder->setLabel(m_passLabels[kCullPass]);
        encoder->setArgumentTable(lists.table);
        encoder->setComputePipelineState(m_pipelines.cull);
        lists.table->setAddress(slot.push(bytesOf(root)), 0);
        encoder->dispatchThreadgroups(MTL::Size::Make(kCullGroups.width, kCullGroups.height, kCullGroups.depth), MTL::Size::Make(kCullThreadsPerGroup, 1, 1));
        encoder->endEncoding();
        calls.record += 7;
        endPass(commands, heap, 1);
        commands->endCommandBuffer();
        ++calls.record;
    }

    //==================================================================================================================
    // Pass 2: the camera's counted draw into the G-buffer, its kernel in a compute encoder ahead of the render encoder.
    void NativeFrame::recordGbuffer(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex)
    {
        const size_t list = static_cast<size_t>(FrameList::Gbuffer);
        MTL4::CommandBuffer* commands = lists.lists[list];
        MTL4::CounterHeap* heap = lists.heaps[list];
        NativeCalls& calls = m_context.calls();
        const Transform camera = cameraViewProjection();
        const GeometryRoot root{
            .instances = nativeAddr(m_resources.instances()->gpuAddress()),
            .vertices = nativeAddr(m_resources.vertices()->gpuAddress()),
            .viewProj0 = camera.row0,
            .viewProj1 = camera.row1,
            .viewProj2 = camera.row2,
            .viewProj3 = camera.row3,
        };

        commands->beginCommandBuffer(lists.allocators[list]);
        ++calls.record;
        beginPass(commands, heap, kGeometryPass, 0);
        MTL4::ComputeCommandEncoder* kernels = commands->computeCommandEncoder();
        kernels->setLabel(m_passLabels[kGeometryPass]);
        calls.record += 2;
        m_indirect.encodeKernels(kernels, lists.table, slotIndex, kGeometryDraw, 1);
        kernels->endEncoding();
        ++calls.record;

        MTL4::RenderCommandEncoder* encoder = openRenderPass(commands, kGeometryPass);
        emitBarrier(encoder, FrameBatch::Geometry, calls);
        m_indirect.barrier(encoder);
        setRenderState(encoder, kGeometryPass, m_pipelines.geometry, lists.table);
        lists.table->setAddress(slot.push(bytesOf(root)), 0);
        ++calls.record;
        m_indirect.execute(encoder, slotIndex, kGeometryDraw);
        encoder->endEncoding();
        ++calls.record;
        endPass(commands, heap, 1);
        emitBarrier(nullptr, FrameBatch::GbufferClose, calls);
        commands->endCommandBuffer();
        ++calls.record;
    }

    //==================================================================================================================
    // Pass 3: the occlusion dispatch, the barrier on aoRaw inside the encoder, then the blur.
    void NativeFrame::recordAo(SlotLists& lists, NativeContext::Slot& slot)
    {
        const size_t list = static_cast<size_t>(FrameList::Ao);
        MTL4::CommandBuffer* commands = lists.lists[list];
        MTL4::CounterHeap* heap = lists.heaps[list];
        NativeCalls& calls = m_context.calls();
        const AoRoot aoRoot{
            .depth = m_resources.view(FrameTexture::Depth),
            .normal = m_resources.view(FrameTexture::Normal),
            .aoRaw = m_resources.view(FrameTexture::AoRaw),
            .width = kFrameWidth,
            .height = kFrameHeight,
            .pad0 = 0,
            .pad1 = 0,
            .pad2 = 0,
        };
        const BlurRoot blurRoot{
            .aoRaw = m_resources.view(FrameTexture::AoRaw),
            .ao = m_resources.view(FrameTexture::Ao),
            .width = kFrameWidth,
            .height = kFrameHeight,
        };
        const MTL::Size groups = MTL::Size::Make(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        const MTL::Size group = MTL::Size::Make(kImageThreadsPerGroup.width, kImageThreadsPerGroup.height, kImageThreadsPerGroup.depth);

        commands->beginCommandBuffer(lists.allocators[list]);
        ++calls.record;
        emitBarrier(nullptr, FrameBatch::AoStart, calls);
        beginPass(commands, heap, kAoPass, 0);
        MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
        encoder->setLabel(m_passLabels[kAoPass]);
        encoder->setArgumentTable(lists.table);
        encoder->setComputePipelineState(m_pipelines.ao);
        lists.table->setAddress(slot.push(bytesOf(aoRoot)), 0);
        encoder->dispatchThreadgroups(groups, group);
        calls.record += 6;
        emitBarrier(encoder, FrameBatch::AoRaw, calls);
        encoder->setComputePipelineState(m_pipelines.blur);
        lists.table->setAddress(slot.push(bytesOf(blurRoot)), 0);
        encoder->dispatchThreadgroups(groups, group);
        encoder->endEncoding();
        calls.record += 4;
        endPass(commands, heap, 1);
        emitBarrier(nullptr, FrameBatch::AoClose, calls);
        commands->endCommandBuffer();
        ++calls.record;
    }

    //==================================================================================================================
    // Passes 4 to 6: each cascade's counted draw into its layer of `shadow`.
    void NativeFrame::recordShadows(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex)
    {
        const size_t list = static_cast<size_t>(FrameList::Shadows);
        MTL4::CommandBuffer* commands = lists.lists[list];
        MTL4::CounterHeap* heap = lists.heaps[list];
        NativeCalls& calls = m_context.calls();

        commands->beginCommandBuffer(lists.allocators[list]);
        ++calls.record;
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            const uint32_t pass = kShadowPass + cascade;
            const Transform view = cascadeViewProjection(cascade);
            const ShadowRoot root{
                .instances = nativeAddr(m_resources.instances()->gpuAddress()),
                .vertices = nativeAddr(m_resources.vertices()->gpuAddress()),
                .viewProj0 = view.row0,
                .viewProj1 = view.row1,
                .viewProj2 = view.row2,
                .viewProj3 = view.row3,
            };
            beginPass(commands, heap, pass, 2 * cascade);
            MTL4::ComputeCommandEncoder* kernels = commands->computeCommandEncoder();
            kernels->setLabel(m_passLabels[pass]);
            calls.record += 2;
            m_indirect.encodeKernels(kernels, lists.table, slotIndex, kShadowDraw + cascade, 1);
            kernels->endEncoding();
            ++calls.record;

            MTL4::RenderCommandEncoder* encoder = openRenderPass(commands, pass);
            if (cascade == 0)
            {
                emitBarrier(encoder, FrameBatch::Shadows, calls);
            }
            m_indirect.barrier(encoder);
            setRenderState(encoder, pass, m_pipelines.shadow, lists.table);
            lists.table->setAddress(slot.push(bytesOf(root)), 0);
            ++calls.record;
            m_indirect.execute(encoder, slotIndex, kShadowDraw + cascade);
            encoder->endEncoding();
            ++calls.record;
            endPass(commands, heap, 2 * cascade + 1);
        }
        commands->endCommandBuffer();
        ++calls.record;
    }

    //==================================================================================================================
    // Passes 7 to 14: lighting, the camera upload, the forward boxes, bloom, tone map and the copy of row 540.
    void NativeFrame::recordLighting(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex)
    {
        const size_t list = static_cast<size_t>(FrameList::Lighting);
        MTL4::CommandBuffer* commands = lists.lists[list];
        MTL4::CounterHeap* heap = lists.heaps[list];
        MTL4::ArgumentTable* table = lists.table;
        NativeCalls& calls = m_context.calls();
        const Transform inverse = cameraInverseViewProjection();
        const std::array<Transform, kCascadeCount> cascades{ cascadeViewProjection(0), cascadeViewProjection(1), cascadeViewProjection(2) };
        const LightingRoot lightingRoot{
            .albedo = m_resources.view(FrameTexture::Albedo),
            .normal = m_resources.view(FrameTexture::Normal),
            .material = m_resources.view(FrameTexture::Material),
            .depth = m_resources.view(FrameTexture::Depth),
            .shadow = m_resources.view(FrameTexture::Shadow),
            .ao = m_resources.view(FrameTexture::Ao),
            .hdr = m_resources.view(FrameTexture::Hdr),
            .width = kFrameWidth,
            .height = kFrameHeight,
            .pad0 = 0,
            .pad1 = 0,
            .pad2 = 0,
            .lightDirection = lightDirection(),
            .inverseViewProj0 = inverse.row0,
            .inverseViewProj1 = inverse.row1,
            .inverseViewProj2 = inverse.row2,
            .inverseViewProj3 = inverse.row3,
            .cascade0Row0 = cascades[0].row0,
            .cascade0Row1 = cascades[0].row1,
            .cascade0Row2 = cascades[0].row2,
            .cascade0Row3 = cascades[0].row3,
            .cascade1Row0 = cascades[1].row0,
            .cascade1Row1 = cascades[1].row1,
            .cascade1Row2 = cascades[1].row2,
            .cascade1Row3 = cascades[1].row3,
            .cascade2Row0 = cascades[2].row0,
            .cascade2Row1 = cascades[2].row1,
            .cascade2Row2 = cascades[2].row2,
            .cascade2Row3 = cascades[2].row3,
        };

        commands->beginCommandBuffer(lists.allocators[list]);
        ++calls.record;

        // 7 lighting.
        beginPass(commands, heap, kLightingPass, 0);
        MTL4::ComputeCommandEncoder* lighting = commands->computeCommandEncoder();
        lighting->setLabel(m_passLabels[kLightingPass]);
        calls.record += 2;
        emitBarrier(lighting, FrameBatch::Lighting, calls);
        lighting->setArgumentTable(table);
        lighting->setComputePipelineState(m_pipelines.lighting);
        table->setAddress(slot.push(bytesOf(lightingRoot)), 0);
        lighting->dispatchThreadgroups(
            MTL::Size::Make(kImageGroups.width, kImageGroups.height, kImageGroups.depth),
            MTL::Size::Make(kImageThreadsPerGroup.width, kImageThreadsPerGroup.height, kImageThreadsPerGroup.depth)
        );
        lighting->endEncoding();
        calls.record += 5;
        endPass(commands, heap, 1);

        // The camera's view-projection, read by every forward box.
        const Transform camera = cameraViewProjection();
        const uint64_t cameraAddress = slot.push(bytesOf(camera));

        // 8 forward: 500 boxes, each a root and a direct indexed draw of its mesh.
        beginPass(commands, heap, kForwardPass, 2);
        MTL4::RenderCommandEncoder* forward = openRenderPass(commands, kForwardPass);
        emitBarrier(forward, FrameBatch::Forward, calls);
        setRenderState(forward, kForwardPass, m_pipelines.forward, table);
        const uint64_t indices = m_resources.indices()->gpuAddress();
        const Addr vertices = nativeAddr(m_resources.vertices()->gpuAddress());
        for (uint32_t index = 0; index < kForwardDraws; ++index)
        {
            const ForwardBox box = forwardBox(index);
            const uint32_t mesh = box.box % kMeshCount;
            const ForwardRoot root{
                .transform0 = box.transform.row0,
                .transform1 = box.transform.row1,
                .transform2 = box.transform.row2,
                .transform3 = box.transform.row3,
                .color = box.color,
                .vertices = vertices,
                .camera = nativeAddr(cameraAddress),
            };
            table->setAddress(slot.push(bytesOf(root)), 0);
            forward->drawIndexedPrimitives(
                MTL::PrimitiveTypeTriangle,
                kMeshIndexCount,
                MTL::IndexTypeUInt16,
                indices + uint64_t{ kMeshIndexCount } * mesh * sizeof(uint16_t),
                kMeshIndexCount * sizeof(uint16_t),
                1,
                static_cast<NS::Integer>(kMeshVertexCount * mesh),
                0
            );
            calls.record += 2;
        }
        forward->endEncoding();
        ++calls.record;
        endPass(commands, heap, 3);

        // 9 to 13: bloom down twice, bloom up twice and the tone map, each one full-screen triangle.
        struct Fullscreen
        {
            uint32_t pass;
            FrameBatch batch;
            MTL::RenderPipelineState* pipeline;
            std::span<const std::byte> root;
        };
        const BloomDownRoot down0{ m_resources.view(FrameTexture::Hdr), m_resources.sampler(), kBloomDown0TexelX, kBloomDown0TexelY };
        const BloomDownRoot down1{ m_resources.view(FrameTexture::Bloom0), m_resources.sampler(), kBloomDown1TexelX, kBloomDown1TexelY };
        const BloomUpRoot up1{ m_resources.view(FrameTexture::Bloom1), m_resources.sampler(), kBloomUpIntensity, 0 };
        const BloomUpRoot up0{ m_resources.view(FrameTexture::Bloom0), m_resources.sampler(), kBloomUpIntensity, 0 };
        const ToneMapRoot toneMap{ m_resources.view(FrameTexture::Hdr), m_resources.sampler(), kToneMapExposure, 0 };
        const std::array<Fullscreen, 5> post{ {
            { kBloomDown0Pass, FrameBatch::BloomDown0, m_pipelines.bloomDown, bytesOf(down0) },
            { kBloomDown1Pass, FrameBatch::BloomDown1, m_pipelines.bloomDown, bytesOf(down1) },
            { kBloomUp1Pass, FrameBatch::BloomUp1, m_pipelines.bloomUp, bytesOf(up1) },
            { kBloomUp0Pass, FrameBatch::BloomUp0, m_pipelines.bloomUp, bytesOf(up0) },
            { kToneMapPass, FrameBatch::ToneMap, m_pipelines.toneMap, bytesOf(toneMap) },
        } };
        uint32_t entry = 4;
        for (const Fullscreen& pass : post)
        {
            beginPass(commands, heap, pass.pass, entry);
            MTL4::RenderCommandEncoder* encoder = openRenderPass(commands, pass.pass);
            emitBarrier(encoder, pass.batch, calls);
            setRenderState(encoder, pass.pass, pass.pipeline, table);
            drawFullscreen(encoder, table, slot.push(pass.root));
            encoder->endEncoding();
            ++calls.record;
            endPass(commands, heap, entry + 1);
            entry += 2;
        }

        // 14 readback: row 540 of ldr into readback[s].
        beginPass(commands, heap, kReadbackPass, entry);
        MTL4::ComputeCommandEncoder* copy = commands->computeCommandEncoder();
        copy->setLabel(m_passLabels[kReadbackPass]);
        calls.record += 2;
        emitBarrier(copy, FrameBatch::Readback, calls);
        copy->copyFromTexture(
            m_resources.texture(FrameTexture::Ldr),
            0,
            0,
            MTL::Origin::Make(0, kReadbackRow, 0),
            MTL::Size::Make(kFrameWidth, 1, 1),
            m_resources.readback(slotIndex),
            0,
            kReadbackBytes,
            kReadbackBytes
        );
        copy->endEncoding();
        calls.record += 2;
        endPass(commands, heap, entry + 1);
        commands->endCommandBuffer();
        ++calls.record;
    }

    //==================================================================================================================
    // The five submissions in order. A wait holds every later commit of its queue; a signal follows the commits before
    // it, so the shadows overlap the occlusion on the compute queue.
    void NativeFrame::submit(const SlotLists& lists, uint64_t value)
    {
        NativeCalls& calls = m_context.calls();
        MTL4::CommandQueue* graphics = m_context.graphicsQueue();
        MTL4::CommandQueue* compute = m_context.computeQueue();
        const auto one = [&](FrameList list) { return std::span<MTL4::CommandBuffer* const>(&lists.lists[static_cast<size_t>(list)], 1); };

        m_context.commit(compute, one(FrameList::Cull));
        compute->signalEvent(m_cullDone, value);
        graphics->wait(m_cullDone, value);
        m_context.commit(graphics, one(FrameList::Gbuffer));
        graphics->signalEvent(m_gbufferDone, value);
        compute->wait(m_gbufferDone, value);
        m_context.commit(compute, one(FrameList::Ao));
        compute->signalEvent(m_aoDone, value);
        m_context.commit(graphics, one(FrameList::Shadows));
        graphics->wait(m_aoDone, value);
        m_context.commit(graphics, one(FrameList::Lighting));
        calls.submit += 6;
    }

    //==================================================================================================================
    // Every list is recorded before the first submission, so no commit waits on the recording of a later list and the
    // GPU span holds GPU work only.
    std::expected<FrameResult, std::string> NativeFrame::frame(uint32_t index)
    {
        const uint32_t slotIndex = slotOf(index);
        NativeContext::Slot& slot = m_context.beginFrame(index, kListCount);
        SlotLists& lists = m_slots[slotIndex];
        resolve(lists);
        // Allocators are not native-call targets, so their resets count nowhere.
        for (MTL4::CommandAllocator* allocator : lists.allocators)
        {
            allocator->reset();
        }

        recordCull(lists, slot, slotIndex);
        recordGbuffer(lists, slot, slotIndex);
        recordAo(lists, slot);
        recordShadows(lists, slot, slotIndex);
        recordLighting(lists, slot, slotIndex);
        submit(lists, uint64_t{ index } + 1);

        const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
        if (!gpu)
        {
            return std::unexpected(gpu.error());
        }
        lists.written = true;
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
        return m_resources.readOutputs(slotOf(*m_lastIndex), *m_lastIndex);
    }

    //==================================================================================================================
    ArmResult runNativeFrame(const Config& config, const RunOptions& options)
    {
        return runArm<NativeFrame>(config, options);
    }
} // namespace rojoRHI::bench
