//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommandList.cpp
/// @brief Encodes render and compute passes, bindings, draws, dispatches, and barriers.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4CommandList.h"

#include "Core/Assert.h"
#include "Metal4Resources.h"
#include "RHI/CaptureSchema.h"
#include "RHI/Indirect.h"
#include "RHI/Validate.h"

#include <cstring>

namespace lmx::rhi::metal4 {
namespace {

// A render pass consumes a dependency at the earliest render stage a barrier can name, not at the
// fragment stage: vertex pulling reads its buffer there, and the arguments of an indirect draw are
// fetched before either stage runs. Naming only StageFragment would let those reads pass the
// barrier.
constexpr MTL::Stages kRenderStages = MTL::StageVertex | MTL::StageFragment;

// Which stage a copy runs in. Metal 4 has no blit encoder -- copies are recorded on a compute
// encoder -- and the public headers do not say whether those retire in StageBlit (the stage that
// exists for exactly this work and has no other encoder left to describe) or in StageDispatch
// alongside the encoder's dispatches. Naming both is correct under either answer, and the cost of
// being wrong in the other direction is a missed dependency rather than a slow one.
constexpr MTL::Stages kCopyStages = MTL::StageBlit | MTL::StageDispatch;

//======================================================================================================================
// Which queue stage performs a use. Metal 4 barriers name stages rather than resources, so this is
// the whole translation from the RHI's use vocabulary to what a barrier can express: attachment
// writes and shader reads happen in the fragment stage, storage access in a dispatch.
MTL::Stages stagesOf(TextureUse use) {
    switch (use) {
    case TextureUse::ExternalRead:
    case TextureUse::ExternalWrite:
        return MTL::StageAll;
    case TextureUse::RenderTarget:
        return MTL::StageFragment;
    case TextureUse::ShaderRead:
        // bindTexture is valid in both render and compute passes. A barrier carries the access
        // kind, not the producer pass kind, so include dispatch rather than guessing which one read
        // it.
        return kRenderStages | MTL::StageDispatch;
    case TextureUse::StorageRead:
    case TextureUse::StorageWrite:
        return MTL::StageDispatch;
    case TextureUse::CopySource:
    case TextureUse::CopyDestination:
        return kCopyStages;
    }
    return MTL::StageAll;
}

//======================================================================================================================
// The same translation for buffers. A shader read of a buffer is a vertex or fragment stage read
// -- vertex pulling, indices, uniforms -- and indirect arguments are fetched when the draw or
// dispatch is issued, which is the earliest stage of whichever encoder consumes them.
MTL::Stages stagesOf(BufferUse use) {
    switch (use) {
    case BufferUse::ShaderRead:
        // bindBuffer and bindFrameData are also valid in compute passes.
        return kRenderStages | MTL::StageDispatch;
    case BufferUse::StorageRead:
    case BufferUse::StorageWrite:
        return MTL::StageDispatch;
    case BufferUse::CopySource:
    case BufferUse::CopyDestination:
        return kCopyStages;
    case BufferUse::IndirectArgument:
        return MTL::StageVertex | MTL::StageDispatch;
    }
    return MTL::StageAll;
}

//======================================================================================================================
bool isWrite(TextureUse use) {
    return use == TextureUse::ExternalWrite || use == TextureUse::RenderTarget ||
           use == TextureUse::StorageWrite || use == TextureUse::CopyDestination;
}

//======================================================================================================================
bool isWrite(BufferUse use) {
    return use == BufferUse::StorageWrite || use == BufferUse::CopyDestination;
}

//======================================================================================================================
MTL::Origin originOf(const TextureCopyRegion& region) {
    return MTL::Origin::Make(region.x, region.y, region.z);
}

//======================================================================================================================
MTL::Size extentOf(const TextureCopyRegion& region) {
    return MTL::Size::Make(region.width, region.height, region.depth);
}

} // namespace

//======================================================================================================================
Metal4FrameTimestamps::~Metal4FrameTimestamps() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    heap.reset();
}

//======================================================================================================================
void Metal4CommandList::beginTimedPass(std::string_view label) {
    // Timestamps are written on the command buffer rather than inside the encoder, which is what
    // makes them pass boundaries: an encoder-stage timestamp would be ordered against a shader
    // stage, and a render pass's load and clear work happens before any stage runs. Both writes of
    // a pass therefore straddle the encoder -- this one, and the one endTimedPass makes after
    // endEncoding.
    const size_t passIndex = m_timestamps->passLabels.size();
    LMX_ASSERT(passIndex < kMaxTimedPassesPerFrame,
               "beginPass: this frame has more passes than the per-frame timestamp heap holds -- "
               "grow kMaxTimedPassesPerFrame");
    m_timestamps->passLabels.emplace_back(label);
    m_commandBuffer->writeTimestampIntoHeap(m_timestamps->heap.get(), passIndex * 2);
}

//======================================================================================================================
void Metal4CommandList::endTimedPass() {
    // Closes the pair beginTimedPass opened; the label pushed there names this index.
    const size_t passIndex = m_timestamps->passLabels.size() - 1;
    m_commandBuffer->writeTimestampIntoHeap(m_timestamps->heap.get(), passIndex * 2 + 1);
}

//======================================================================================================================
void Metal4CommandList::emitPendingBarrier(MTL4::CommandEncoder* encoder,
                                           MTL::Stages consumerStages) {
    // Metal barriers are encoder operations, so a between-pass RHI barrier is emitted by the
    // consumer encoder as its first command. afterQueueStages names the producing stages, which are
    // queue-scoped and so reach back across every earlier encoder (earlier frames included, since
    // the queue outlives a frame); beforeStages is this encoder's own stage class, because the pass
    // that opens is the barrier's consumer by construction and no other encoder's stages are named.
    // That asymmetry is the model rhi::CommandList::textureBarrier states: the producing side is as
    // wide as the queue, the consuming side is exactly this pass, so a later pass of a different
    // kind is ordered by this barrier only where its stages happen to coincide, and callers owe it
    // one of its own instead of relying on that.
    if (m_pendingTemporalFence != nullptr) {
        encoder->waitForFence(m_pendingTemporalFence, consumerStages);
        m_pendingTemporalFence = nullptr;
    }
    if (m_pendingBarrierStages == MTL::Stages{}) {
        return;
    }
    encoder->barrierAfterQueueStages(m_pendingBarrierStages, consumerStages,
                                     m_pendingBarrierVisibility);
    m_pendingBarrierStages = MTL::Stages{};
    m_pendingBarrierVisibility = MTL4::VisibilityOptions{};
}

//======================================================================================================================
void Metal4CommandList::beginRenderPass(const RenderPassDesc& desc) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // endFrameReset clears every per-frame pointer, making them the open-frame sentinel.
    LMX_ASSERT(m_argumentTable != nullptr && m_frameArena != nullptr && m_timestamps != nullptr,
               "beginRenderPass: no frame is open -- this command list is only valid between "
               "Device::beginFrame and Device::endFrame");
    LMX_ASSERT(!inPass(), "beginRenderPass: a pass is already open on this command list");
    const Result<void> targets = validateRenderPassTargets(desc.colorTarget, desc.depthTarget);
    LMX_ASSERT(targets.has_value(), targets.error().message);
    const Result<void> extras =
        validateExtraColorTargets(desc.colorTarget, desc.extraColor, desc.extraColorCount);
    LMX_ASSERT(extras.has_value(), extras.error().message);
    const Result<void> area = validateRenderArea(desc.colorTarget, desc.depthTarget,
                                                 desc.renderAreaWidth, desc.renderAreaHeight);
    LMX_ASSERT(area.has_value(), area.error().message);
    // Discarding the only attachment would make a depth-only pass produce no observable output.
    LMX_ASSERT(desc.colorTarget != nullptr || desc.storeDepth,
               "RenderPassDesc: a depth-only pass must set storeDepth -- it has no other output, "
               "so discarding depth would make the whole pass dead work");

    const std::string_view label = desc.label.empty() ? "lmx.pass.unnamed" : desc.label;

    auto* colorTarget = static_cast<Metal4Texture*>(desc.colorTarget);

    auto passDesc = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
    // Metal represents a depth-only pass with an unset color attachment.
    if (colorTarget != nullptr) {
        MTL::RenderPassColorAttachmentDescriptor* color = passDesc->colorAttachments()->object(0);
        color->setTexture(colorTarget->handle());
        color->setLoadAction(desc.clear ? MTL::LoadActionClear : MTL::LoadActionLoad);
        color->setStoreAction(MTL::StoreActionStore);
        if (desc.clear) {
            color->setClearColor(MTL::ClearColor(desc.clearColor[0], desc.clearColor[1],
                                                 desc.clearColor[2], desc.clearColor[3]));
        }
    }

    // Extra i is attachment i + 1: colorTarget occupies attachment zero.
    for (uint32_t i = 0; i < desc.extraColorCount; ++i) {
        const ExtraColorTarget& extra = desc.extraColor[i];
        auto* extraTarget = static_cast<Metal4Texture*>(extra.target);
        // ResourceID hides usage from Metal validation, so reject a non-attachment texture here.
        LMX_ASSERT((extraTarget->handle()->usage() & MTL::TextureUsageRenderTarget) != 0,
                   "RenderPassDesc.extraColor: texture has no render-target usage -- create it "
                   "with renderTarget = true");

        MTL::RenderPassColorAttachmentDescriptor* attachment =
            passDesc->colorAttachments()->object(i + 1);
        attachment->setTexture(extraTarget->handle());
        attachment->setLoadAction(extra.clear ? MTL::LoadActionClear : MTL::LoadActionLoad);
        attachment->setStoreAction(MTL::StoreActionStore);
        if (extra.clear) {
            attachment->setClearColor(MTL::ClearColor(extra.clearColor[0], extra.clearColor[1],
                                                      extra.clearColor[2], extra.clearColor[3]));
        }
    }

    if (desc.depthTarget != nullptr) {
        auto* depthTarget = static_cast<Metal4Texture*>(desc.depthTarget);
        // Validate before Metal's pass validator aborts without identifying the RHI call.
        LMX_ASSERT(depthTarget->handle()->pixelFormat() == MTL::PixelFormatDepth32Float,
                   "RenderPassDesc.depthTarget must be a D32Float texture");
        // No RHI pass currently establishes ownership of depth contents for LoadActionLoad.
        LMX_ASSERT(desc.clear,
                   "RenderPassDesc: a depth attachment requires clear -- no pass loads depth, so "
                   "a load would read memory this pass never wrote");

        MTL::RenderPassDepthAttachmentDescriptor* depth = passDesc->depthAttachment();
        depth->setTexture(depthTarget->handle());
        depth->setLoadAction(desc.clear ? MTL::LoadActionClear : MTL::LoadActionLoad);
        depth->setStoreAction(desc.storeDepth ? MTL::StoreActionStore : MTL::StoreActionDontCare);
        depth->setClearDepth(desc.clearDepth);
    }

    passDesc->setDefaultRasterSampleCount(1);

    beginTimedPass(label);

    m_encoder = NS::RetainPtr(m_commandBuffer->renderCommandEncoder(passDesc.get()));
    LMX_ASSERT(m_encoder, "beginRenderPass: failed to create a render command encoder");
    m_encoder->setLabel(makeString(label).get());

    emitPendingBarrier(m_encoder.get(), kRenderStages);

    // Set the viewport explicitly so later sub-region passes cannot inherit an attachment-derived
    // default that no longer matches their render area.
    const Texture& extentSource =
        desc.colorTarget != nullptr ? *desc.colorTarget : *desc.depthTarget;
    const MTL::Viewport viewport{0.0,
                                 0.0,
                                 static_cast<double>(extentSource.width()),
                                 static_cast<double>(extentSource.height()),
                                 0.0,
                                 1.0};
    m_encoder->setViewport(viewport);

    // A render area draws into the attachment's origin-anchored corner: the viewport shrinks to it,
    // and the scissor keeps a shader that ignores the viewport from writing the texels outside it.
    // The full-area path sets no scissor at all, so passes that never ask for one encode exactly
    // what they did before.
    if (desc.renderAreaWidth != 0) {
        const MTL::Viewport areaViewport{0.0,
                                         0.0,
                                         static_cast<double>(desc.renderAreaWidth),
                                         static_cast<double>(desc.renderAreaHeight),
                                         0.0,
                                         1.0};
        m_encoder->setViewport(areaViewport);
        m_encoder->setScissorRect(
            MTL::ScissorRect{0, 0, desc.renderAreaWidth, desc.renderAreaHeight});
    }

    m_encoder->setArgumentTable(m_argumentTable, MTL::RenderStageVertex | MTL::RenderStageFragment);
}

//======================================================================================================================
void Metal4CommandList::bindPipeline(GraphicsPipeline& pipeline) {
    LMX_ASSERT(m_encoder, "bindPipeline must be called between beginRenderPass and endRenderPass");
    auto& metalPipeline = static_cast<Metal4Pipeline&>(pipeline);
    m_encoder->setRenderPipelineState(metalPipeline.handle());
    // Metal keeps depth state separate; null preserves its compare-always/no-write default.
    if (MTL::DepthStencilState* depthState = metalPipeline.depthState()) {
        m_encoder->setDepthStencilState(depthState);
    }
    // Replay all raster state because Metal encoder state persists across pipeline binds.
    const Metal4RasterState& raster = metalPipeline.rasterState();
    m_encoder->setTriangleFillMode(raster.fillMode);
    m_encoder->setCullMode(raster.cullMode);
    m_encoder->setFrontFacingWinding(raster.frontFacingWinding);
    m_encoder->setDepthBias(raster.depthBias, raster.slopeScale, raster.biasClamp);
}

//======================================================================================================================
void Metal4CommandList::bindBuffer(uint32_t slot, Buffer& buffer) {
    LMX_ASSERT(inShaderPass(), "bindBuffer must be called inside a render or compute pass");
    LMX_ASSERT(slot < CommandList::kMaxBufferBindings,
               "bindBuffer: slot exceeds the argument table's buffer binding count");

    // Argument tables hold raw addresses; ResidencyRegistration keeps the allocation resident.
    m_argumentTable->setAddress(static_cast<Metal4Buffer&>(buffer).handle()->gpuAddress(), slot);
}

//======================================================================================================================
void Metal4CommandList::bindTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view) {
    LMX_ASSERT(inShaderPass(), "bindTexture must be called inside a render or compute pass");
    LMX_ASSERT(slot < CommandList::kMaxTextureBindings,
               "bindTexture: slot exceeds the argument table's texture binding count");
    auto& metalTexture = static_cast<Metal4Texture&>(texture);
    const Result<void> viewOk = validateTextureView(texture, view);
    LMX_ASSERT(viewOk.has_value(), viewOk.error().message);
    // ResourceID hides usage from Metal validation, so reject non-readable textures before bind.
    LMX_ASSERT((metalTexture.handle()->usage() & MTL::TextureUsageShaderRead) != 0,
               "bindTexture: texture has no ShaderRead usage -- create it with sampled = true or "
               "storageRead = true or cpuReadback = true");
    // Texture, buffer, and sampler slots occupy separate arrays in the argument table.
    m_argumentTable->setTexture(metalTexture.viewFor(view)->gpuResourceID(), slot);
}

//======================================================================================================================
void Metal4CommandList::bindSampler(uint32_t slot, Sampler& sampler) {
    LMX_ASSERT(inShaderPass(), "bindSampler must be called inside a render or compute pass");
    LMX_ASSERT(slot < CommandList::kMaxSamplerBindings,
               "bindSampler: slot exceeds the argument table's sampler binding count");
    // Samplers use ResourceID but need no residency registration because they are not allocations.
    m_argumentTable->setSamplerState(static_cast<Metal4Sampler&>(sampler).handle()->gpuResourceID(),
                                     slot);
}

//======================================================================================================================
// One allocation, one copy, one address bind, and no native object touched beyond the argument
// table: everything the page contributes -- its mapped base, its GPU base and its label -- was
// resolved when the page was created, which is what keeps this free of resource creation and
// property queries once a slot has reached its high water.
GpuAddress Metal4CommandList::bindFrameData(uint32_t slot, const void* data, uint64_t size,
                                            uint64_t alignment) {
    LMX_ASSERT(inShaderPass(),
               "bindFrameData must be called inside a render or compute pass -- a copy pass has no "
               "argument table and therefore no slot to bind into");
    const Result<void> request = validateFrameData(slot, data, size, alignment);
    LMX_ASSERT(request.has_value(), request.error().message);

    const Metal4FrameDataBlock block = m_frameArena->allocate(size, alignment);

    // Recorded only during capture. The page label is borrowed from the arena, so this needs no
    // autorelease pool and no Metal property read.
    if (debug::CaptureSchema::instance().recordingUploads()) {
        debug::CaptureSchema::instance().recordFrameDataUpload(
            {.pageLabel = std::string(m_frameArena->pageLabel(block.pageIndex)),
             .slot = slot,
             .pageOffset = block.pageOffset,
             .sizeBytes = size,
             .alignmentBytes = alignment,
             .gpuAddress = block.gpuAddress});
    }

    std::memcpy(block.cpu, data, size);
    m_argumentTable->setAddress(block.gpuAddress, slot);

    ++m_frameDataTally->calls;
    m_frameDataTally->bytes += size;
    ++m_frameDataTally->addressBinds;
    return GpuAddress{block.gpuAddress};
}

//======================================================================================================================
void Metal4CommandList::draw(uint32_t vertexCount, uint32_t firstVertex) {
    LMX_ASSERT(m_encoder, "draw must be called between beginRenderPass and endRenderPass");
    LMX_ASSERT(vertexCount > 0, "draw: vertexCount must be greater than zero");
    m_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, firstVertex, vertexCount);
}

//======================================================================================================================
void Metal4CommandList::drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex) {
    LMX_ASSERT(m_encoder, "drawIndexed must be called between beginRenderPass and endRenderPass");
    LMX_ASSERT(indexCount > 0, "drawIndexed: indexCount must be greater than zero");
    auto& mtlBuffer = static_cast<Metal4Buffer&>(indexBuffer);
    const uint64_t offsetBytes = uint64_t{firstIndex} * sizeof(uint32_t);
    const uint64_t lengthBytes = mtlBuffer.handle()->length();
    LMX_ASSERT(offsetBytes + uint64_t{indexCount} * sizeof(uint32_t) <= lengthBytes,
               "drawIndexed: index range reads past the end of the index buffer");
    // Metal expects the byte count remaining at the offset address, not the full buffer length.
    m_encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, indexCount, MTL::IndexTypeUInt32,
                                     mtlBuffer.handle()->gpuAddress() + offsetBytes,
                                     lengthBytes - offsetBytes);
}

//======================================================================================================================
void Metal4CommandList::drawIndirect(Buffer& argumentBuffer, uint64_t offset) {
    LMX_ASSERT(m_encoder, "drawIndirect must be called between beginRenderPass and endRenderPass");
    const Result<void> argsOk =
        validateIndirectArgs(argumentBuffer, offset, sizeof(DrawIndirectArgs));
    LMX_ASSERT(argsOk.has_value(), argsOk.error().message);
    // Metal 4 takes the arguments by GPU address, so the RHI's byte offset is plain pointer
    // arithmetic rather than a separate encoder parameter.
    m_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                              static_cast<Metal4Buffer&>(argumentBuffer).handle()->gpuAddress() +
                                  offset);
}

//======================================================================================================================
void Metal4CommandList::drawIndexedIndirect(Buffer& indexBuffer, Buffer& argumentBuffer,
                                            uint64_t offset) {
    LMX_ASSERT(m_encoder,
               "drawIndexedIndirect must be called between beginRenderPass and endRenderPass");
    const Result<void> argsOk =
        validateIndirectArgs(argumentBuffer, offset, sizeof(DrawIndexedIndirectArgs));
    LMX_ASSERT(argsOk.has_value(), argsOk.error().message);
    auto& indices = static_cast<Metal4Buffer&>(indexBuffer);
    // The first index lives in the arguments, so the whole index buffer is what the draw is given
    // -- unlike drawIndexed, which folds its firstIndex into the address it passes.
    m_encoder->drawIndexedPrimitives(
        MTL::PrimitiveTypeTriangle, MTL::IndexTypeUInt32, indices.handle()->gpuAddress(),
        indices.handle()->length(),
        static_cast<Metal4Buffer&>(argumentBuffer).handle()->gpuAddress() + offset);
}

//======================================================================================================================
void Metal4CommandList::endRenderPass() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_encoder, "endRenderPass: no render pass is open on this command list");
    m_encoder->endEncoding();
    m_encoder.reset();

    endTimedPass();
}

//======================================================================================================================
void Metal4CommandList::beginComputePass(std::string_view label) {
    // computeCommandEncoder() returns an autoreleased (+0) object, exactly like the render
    // encoder, so this pool is load-bearing rather than symmetric.
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_argumentTable != nullptr && m_frameArena != nullptr && m_timestamps != nullptr,
               "beginComputePass: no frame is open -- this command list is only valid between "
               "Device::beginFrame and Device::endFrame");
    LMX_ASSERT(!inPass(), "beginComputePass: a pass is already open on this command list");

    const std::string_view passLabel = label.empty() ? "lmx.pass.unnamed" : label;

    beginTimedPass(passLabel);

    m_computeEncoder = NS::RetainPtr(m_commandBuffer->computeCommandEncoder());
    LMX_ASSERT(m_computeEncoder, "beginComputePass: failed to create a compute command encoder");
    m_computeEncoder->setLabel(makeString(passLabel).get());

    emitPendingBarrier(m_computeEncoder.get(), MTL::StageDispatch);

    m_computeEncoder->setArgumentTable(m_argumentTable);
}

//======================================================================================================================
void Metal4CommandList::bindComputePipeline(ComputePipeline& pipeline) {
    LMX_ASSERT(m_computeEncoder,
               "bindComputePipeline must be called between beginComputePass and endComputePass");
    auto& metalPipeline = static_cast<Metal4ComputePipeline&>(pipeline);
    m_computeEncoder->setComputePipelineState(metalPipeline.handle());
    // Metal takes the threadgroup shape at dispatch, not at bind, so the bound pipeline is
    // remembered until the pass ends.
    m_computePipeline = &metalPipeline;
}

//======================================================================================================================
void Metal4CommandList::bindStorageBuffer(uint32_t slot, Buffer& buffer, StorageAccess access) {
    LMX_ASSERT(m_computeEncoder,
               "bindStorageBuffer must be called between beginComputePass and endComputePass");
    LMX_ASSERT(slot < CommandList::kMaxBufferBindings,
               "bindStorageBuffer: slot exceeds the argument table's buffer binding count");
    auto& metalBuffer = static_cast<Metal4Buffer&>(buffer);
    // Metal buffers carry no usage bits, so the desc flags are the only record of what the caller
    // meant this allocation to be -- and the only place a mismatch can be caught at all.
    const bool reads = access == StorageAccess::Read || access == StorageAccess::ReadWrite;
    const bool writes = access == StorageAccess::Write || access == StorageAccess::ReadWrite;
    LMX_ASSERT(!reads || metalBuffer.storageRead(),
               "bindStorageBuffer: buffer was not created with BufferDesc.storageRead");
    LMX_ASSERT(!writes || metalBuffer.storageWrite(),
               "bindStorageBuffer: buffer was not created with BufferDesc.storageWrite");
    // Argument tables hold raw addresses; ResidencyRegistration keeps the allocation resident.
    m_argumentTable->setAddress(metalBuffer.handle()->gpuAddress(), slot);
}

//======================================================================================================================
void Metal4CommandList::bindStorageTexture(uint32_t slot, Texture& texture,
                                           const TextureViewDesc& view, StorageAccess access) {
    LMX_ASSERT(m_computeEncoder,
               "bindStorageTexture must be called between beginComputePass and endComputePass");
    LMX_ASSERT(slot < CommandList::kMaxTextureBindings,
               "bindStorageTexture: slot exceeds the argument table's texture binding count");
    auto& metalTexture = static_cast<Metal4Texture&>(texture);
    const Result<void> viewOk = validateTextureView(texture, view);
    LMX_ASSERT(viewOk.has_value(), viewOk.error().message);
    LMX_ASSERT(isStorageFormat(view.format == Format::Unknown ? texture.format() : view.format),
               "bindStorageTexture: the bound format must be one a storage binding can read and "
               "write (RGBA8Unorm or RGBA16Float)");

    // ResourceID hides usage from Metal validation, so reject a texture the shader's access would
    // fault on before the bind rather than inside the dispatch.
    const MTL::TextureUsage usage = metalTexture.handle()->usage();
    const bool reads = access == StorageAccess::Read || access == StorageAccess::ReadWrite;
    const bool writes = access == StorageAccess::Write || access == StorageAccess::ReadWrite;
    LMX_ASSERT(!reads || (usage & MTL::TextureUsageShaderRead) != 0,
               "bindStorageTexture: texture was not created with TextureDesc.storageRead");
    LMX_ASSERT(!writes || (usage & MTL::TextureUsageShaderWrite) != 0,
               "bindStorageTexture: texture was not created with TextureDesc.storageWrite");

    // Texture, buffer, and sampler slots occupy separate arrays in the argument table.
    m_argumentTable->setTexture(metalTexture.viewFor(view)->gpuResourceID(), slot);
}

//======================================================================================================================
void Metal4CommandList::dispatch(uint32_t threadgroupsX, uint32_t threadgroupsY,
                                 uint32_t threadgroupsZ) {
    LMX_ASSERT(m_computeEncoder,
               "dispatch must be called between beginComputePass and endComputePass");
    LMX_ASSERT(threadgroupsX > 0 && threadgroupsY > 0 && threadgroupsZ > 0,
               "dispatch: every threadgroup count must be greater than zero");
    LMX_ASSERT(m_computePipeline != nullptr,
               "dispatch: no compute pipeline is bound -- call bindComputePipeline first");
    m_computeEncoder->dispatchThreadgroups(
        MTL::Size::Make(threadgroupsX, threadgroupsY, threadgroupsZ),
        m_computePipeline->threadsPerThreadgroup());
}

//======================================================================================================================
void Metal4CommandList::dispatchIndirect(Buffer& argumentBuffer, uint64_t offset) {
    LMX_ASSERT(m_computeEncoder,
               "dispatchIndirect must be called between beginComputePass and endComputePass");
    LMX_ASSERT(m_computePipeline != nullptr,
               "dispatchIndirect: no compute pipeline is bound -- call bindComputePipeline first");
    const Result<void> argsOk =
        validateIndirectArgs(argumentBuffer, offset, sizeof(DispatchIndirectArgs));
    LMX_ASSERT(argsOk.has_value(), argsOk.error().message);
    // Only the threadgroup counts come from the buffer; the threads within one still come from the
    // bound pipeline, exactly as they do for the direct dispatch above.
    m_computeEncoder->dispatchThreadgroups(
        static_cast<Metal4Buffer&>(argumentBuffer).handle()->gpuAddress() + offset,
        m_computePipeline->threadsPerThreadgroup());
}

//======================================================================================================================
void Metal4CommandList::endComputePass() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_computeEncoder, "endComputePass: no compute pass is open on this command list");
    m_computeEncoder->endEncoding();
    m_computeEncoder.reset();
    // The pipeline's threadgroup shape belongs to the pass that bound it; carrying it into the
    // next pass would let a dispatch inherit a shape no bind in that pass ever asked for.
    m_computePipeline = nullptr;

    endTimedPass();
}

//======================================================================================================================
void Metal4CommandList::beginCopyPass(std::string_view label) {
    // computeCommandEncoder() returns an autoreleased (+0) object; see beginComputePass.
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_argumentTable != nullptr && m_frameArena != nullptr && m_timestamps != nullptr,
               "beginCopyPass: no frame is open -- this command list is only valid between "
               "Device::beginFrame and Device::endFrame");
    LMX_ASSERT(!inPass(), "beginCopyPass: a pass is already open on this command list");

    const std::string_view passLabel = label.empty() ? "lmx.pass.unnamed" : label;

    beginTimedPass(passLabel);

    // Metal 4 records copies on a compute encoder. No argument table is set on it: a copy pass has
    // no bindings, so handing it the frame's table would advertise a scope it does not have.
    m_copyEncoder = NS::RetainPtr(m_commandBuffer->computeCommandEncoder());
    LMX_ASSERT(m_copyEncoder, "beginCopyPass: failed to create a copy command encoder");
    m_copyEncoder->setLabel(makeString(passLabel).get());

    emitPendingBarrier(m_copyEncoder.get(), kCopyStages);
}

//======================================================================================================================
void Metal4CommandList::copyBuffer(Buffer& source, uint64_t sourceOffset, Buffer& destination,
                                   uint64_t destinationOffset, uint64_t size) {
    LMX_ASSERT(m_copyEncoder, "copyBuffer must be called between beginCopyPass and endCopyPass");
    const Result<void> copyOk =
        validateBufferCopy(source, sourceOffset, destination, destinationOffset, size);
    LMX_ASSERT(copyOk.has_value(), copyOk.error().message);
    m_copyEncoder->copyFromBuffer(static_cast<Metal4Buffer&>(source).handle(), sourceOffset,
                                  static_cast<Metal4Buffer&>(destination).handle(),
                                  destinationOffset, size);
}

//======================================================================================================================
void Metal4CommandList::copyBufferToTexture(Buffer& source, const BufferTextureLayout& layout,
                                            Texture& destination, const TextureCopyRegion& region) {
    LMX_ASSERT(m_copyEncoder,
               "copyBufferToTexture must be called between beginCopyPass and endCopyPass");
    const Result<void> copyOk = validateBufferTextureCopy(source, layout, destination, region);
    LMX_ASSERT(copyOk.has_value(), copyOk.error().message);
    m_copyEncoder->copyFromBuffer(static_cast<Metal4Buffer&>(source).handle(), layout.offset,
                                  layout.bytesPerRow, layout.bytesPerSlice, extentOf(region),
                                  static_cast<Metal4Texture&>(destination).handle(),
                                  region.arrayLayer, region.mipLevel, originOf(region));
}

//======================================================================================================================
void Metal4CommandList::copyTextureToBuffer(Texture& source, const TextureCopyRegion& region,
                                            Buffer& destination,
                                            const BufferTextureLayout& layout) {
    LMX_ASSERT(m_copyEncoder,
               "copyTextureToBuffer must be called between beginCopyPass and endCopyPass");
    const Result<void> copyOk = validateBufferTextureCopy(destination, layout, source, region);
    LMX_ASSERT(copyOk.has_value(), copyOk.error().message);
    m_copyEncoder->copyFromTexture(static_cast<Metal4Texture&>(source).handle(), region.arrayLayer,
                                   region.mipLevel, originOf(region), extentOf(region),
                                   static_cast<Metal4Buffer&>(destination).handle(), layout.offset,
                                   layout.bytesPerRow, layout.bytesPerSlice);
}

//======================================================================================================================
void Metal4CommandList::copyTexture(Texture& source, const TextureCopyRegion& sourceRegion,
                                    Texture& destination,
                                    const TextureCopyRegion& destinationRegion) {
    LMX_ASSERT(m_copyEncoder, "copyTexture must be called between beginCopyPass and endCopyPass");
    const Result<void> copyOk =
        validateTextureCopy(source, sourceRegion, destination, destinationRegion);
    LMX_ASSERT(copyOk.has_value(), copyOk.error().message);
    // The destination takes an origin only: validation has already established that the two regions
    // describe the same extent, so Metal is given the source's.
    m_copyEncoder->copyFromTexture(
        static_cast<Metal4Texture&>(source).handle(), sourceRegion.arrayLayer,
        sourceRegion.mipLevel, originOf(sourceRegion), extentOf(sourceRegion),
        static_cast<Metal4Texture&>(destination).handle(), destinationRegion.arrayLayer,
        destinationRegion.mipLevel, originOf(destinationRegion));
}

//======================================================================================================================
void Metal4CommandList::fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint8_t value) {
    LMX_ASSERT(m_copyEncoder, "fillBuffer must be called between beginCopyPass and endCopyPass");
    const Result<void> rangeOk = validateBufferBytes(buffer, offset, size);
    LMX_ASSERT(rangeOk.has_value(), rangeOk.error().message);
    m_copyEncoder->fillBuffer(static_cast<Metal4Buffer&>(buffer).handle(),
                              NS::Range::Make(offset, size), value);
}

//======================================================================================================================
void Metal4CommandList::endCopyPass() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_copyEncoder, "endCopyPass: no copy pass is open on this command list");
    m_copyEncoder->endEncoding();
    m_copyEncoder.reset();

    endTimedPass();
}

//======================================================================================================================
void Metal4CommandList::textureBarrier(Texture& texture, const TextureSubresourceRange& range,
                                       TextureUse from, TextureUse to, BarrierOptions options) {
    LMX_ASSERT(!inPass(), "textureBarrier must be called between passes, not inside one");
    // Reject barriers outside a frame so a pending edge cannot leak into the next frame.
    LMX_ASSERT(m_argumentTable != nullptr, "textureBarrier must be called inside a frame");
    const Result<void> rangeOk = validateSubresourceRange(texture, range);
    LMX_ASSERT(rangeOk.has_value(), rangeOk.error().message);
    LMX_ASSERT(isWrite(from) || isWrite(to),
               "textureBarrier: at least one side must be a write -- two reads of the same "
               "contents have no hazard to order");

    // The range is a caller-facing declaration only: Metal 4's barriers order queue *stages*, so
    // the emitted dependency covers everything the producing stage wrote, this texture included.
    // Accumulating the producing stages lets several barriers between the same pair of passes
    // collapse into the single barrier the consuming encoder emits.
    m_pendingBarrierStages |= stagesOf(from);
    m_pendingBarrierVisibility |= MTL4::VisibilityOptionDevice;
    if (hasBarrierOption(options, BarrierOptions::ResourceAlias)) {
        m_pendingBarrierVisibility |= MTL4::VisibilityOptionResourceAlias;
    }
}

//======================================================================================================================
void Metal4CommandList::bufferBarrier(Buffer& buffer, const BufferRange& range, BufferUse from,
                                      BufferUse to, BarrierOptions options) {
    LMX_ASSERT(!inPass(), "bufferBarrier must be called between passes, not inside one");
    // Reject barriers outside a frame so a pending edge cannot leak into the next frame.
    LMX_ASSERT(m_argumentTable != nullptr, "bufferBarrier must be called inside a frame");
    const Result<void> rangeOk = validateBufferRange(buffer, range);
    LMX_ASSERT(rangeOk.has_value(), rangeOk.error().message);
    LMX_ASSERT(isWrite(from) || isWrite(to),
               "bufferBarrier: at least one side must be a write -- two reads of the same "
               "contents have no hazard to order");

    // The range is a caller-facing declaration only, exactly as textureBarrier's is: Metal 4's
    // barriers order queue *stages*, so the emitted dependency covers everything the producing
    // stage wrote, these bytes included.
    m_pendingBarrierStages |= stagesOf(from);
    m_pendingBarrierVisibility |= MTL4::VisibilityOptionDevice;
    if (hasBarrierOption(options, BarrierOptions::ResourceAlias)) {
        m_pendingBarrierVisibility |= MTL4::VisibilityOptionResourceAlias;
    }
}

//======================================================================================================================
void Metal4CommandList::resetForFrame(
    MTL4::ArgumentTable* argumentTable, Metal4FrameArena* frameArena, Metal4FrameDataTally* tally,
    Metal4FrameTimestamps* timestamps,
    std::vector<std::shared_ptr<Metal4TemporalScalerState>>* temporalScalers) {
    // Never retarget per-frame storage while an encoder can still reference the old slot.
    LMX_ASSERT(!inPass(), "resetForFrame: a pass is still open from the previous frame");
    LMX_ASSERT(argumentTable != nullptr, "resetForFrame: argument table must not be null");
    LMX_ASSERT(frameArena != nullptr && tally != nullptr,
               "resetForFrame: the frame's data arena and its tally must not be null");
    LMX_ASSERT(timestamps != nullptr && timestamps->heap,
               "resetForFrame: the frame's timestamp slot must carry a counter heap");
    LMX_ASSERT(timestamps->passLabels.empty(),
               "resetForFrame: the frame's timestamp slot still holds the previous frame's passes");
    m_argumentTable = argumentTable;
    m_frameArena = frameArena;
    m_frameDataTally = tally;
    m_timestamps = timestamps;
    m_temporalScalers = temporalScalers;
}

//======================================================================================================================
void Metal4CommandList::endFrameReset() {
    // A pending barrier at commit is an unconsumed dependency edge, not disposable state.
    LMX_ASSERT(m_pendingBarrierStages == MTL::Stages{},
               "a textureBarrier or bufferBarrier was recorded but no later pass consumed it");
    // Clearing per-frame pointers makes use outside a frame detectable.
    m_argumentTable = nullptr;
    m_frameArena = nullptr;
    m_frameDataTally = nullptr;
    m_computePipeline = nullptr;
    // The slot itself outlives the frame -- the device reads its labels when the frame retires --
    // but this list must not be able to append to it outside a frame.
    m_timestamps = nullptr;
    m_temporalScalers = nullptr;
    m_pendingTemporalFence = nullptr;
    m_pendingBarrierStages = MTL::Stages{};
    m_pendingBarrierVisibility = MTL4::VisibilityOptions{};
}

//======================================================================================================================
MTL4::CommandBuffer* Metal4CommandList::commandBuffer() const {
    LMX_ASSERT(m_encoder, "commandBuffer: no render pass is open -- the command buffer is only "
                          "open for encoding between beginRenderPass and endRenderPass");
    return m_commandBuffer;
}

//======================================================================================================================
MTL4::RenderCommandEncoder* Metal4CommandList::currentEncoder() const {
    LMX_ASSERT(m_encoder, "currentEncoder: no render pass is open -- call beginRenderPass first");
    return m_encoder.get();
}

} // namespace lmx::rhi::metal4
