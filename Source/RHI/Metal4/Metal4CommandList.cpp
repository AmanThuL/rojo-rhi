#include "RHI/Metal4/Metal4CommandList.h"

#include "Core/Align.h"
#include "Core/Assert.h"
#include "RHI/CaptureSchema.h"
#include "RHI/Metal4/Metal4Resources.h"
#include "RHI/Validate.h"

#include <cstring>

namespace lmx::rhi::metal4 {

//======================================================================================================================
void Metal4CommandList::beginRenderPass(const RenderPassDesc& desc) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // endFrameReset clears both pointers, making them the command list's open-frame sentinel.
    LMX_ASSERT(m_argumentTable != nullptr && m_uniformRing != nullptr,
               "beginRenderPass: no frame is open -- this command list is only valid between "
               "Device::beginFrame and Device::endFrame");
    LMX_ASSERT(!m_encoder, "beginRenderPass: a render pass is already open on this command list");
    const Result<void> targets = validateRenderPassTargets(desc.colorTarget, desc.depthTarget);
    LMX_ASSERT(targets.has_value(), targets.error().message);
    // Discarding the only attachment would make a depth-only pass produce no observable output.
    LMX_ASSERT(desc.colorTarget != nullptr || desc.storeDepth,
               "RenderPassDesc: a depth-only pass must set storeDepth -- it has no other output, "
               "so discarding depth would make the whole pass dead work");

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

    m_encoder = NS::RetainPtr(m_commandBuffer->renderCommandEncoder(passDesc.get()));
    LMX_ASSERT(m_encoder, "beginRenderPass: failed to create a render command encoder");
    const std::string_view label = desc.label.empty() ? "lmx.pass.unnamed" : desc.label;
    m_encoder->setLabel(makeString(label).get());

    // Metal barriers are encoder operations, so a between-pass RHI barrier is emitted by the
    // consumer encoder as its first command.
    if (m_pendingBarrier) {
        // Queue stages refer to prior encoders; beforeStages covers every fragment read encoded
        // after this point. Render-target writes also belong to the fragment stage in Metal.
        m_encoder->barrierAfterQueueStages(MTL::StageFragment, MTL::StageFragment,
                                           MTL4::VisibilityOptionDevice);
        m_pendingBarrier = false;
    }

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
    LMX_ASSERT(m_encoder, "bindBuffer must be called between beginRenderPass and endRenderPass");

    // Argument tables hold raw addresses; ResidencyRegistration keeps the allocation resident.
    m_argumentTable->setAddress(static_cast<Metal4Buffer&>(buffer).handle()->gpuAddress(), slot);
}

//======================================================================================================================
void Metal4CommandList::bindTexture(uint32_t slot, Texture& texture) {
    LMX_ASSERT(m_encoder, "bindTexture must be called between beginRenderPass and endRenderPass");
    auto& metalTexture = static_cast<Metal4Texture&>(texture);
    // ResourceID hides usage from Metal validation, so reject non-readable textures before bind.
    LMX_ASSERT((metalTexture.handle()->usage() & MTL::TextureUsageShaderRead) != 0,
               "bindTexture: texture has no ShaderRead usage -- create it with sampled = true or "
               "cpuReadback = true");
    // Texture, buffer, and sampler slots occupy separate arrays in the argument table.
    m_argumentTable->setTexture(metalTexture.handle()->gpuResourceID(), slot);
}

//======================================================================================================================
void Metal4CommandList::bindSampler(uint32_t slot, Sampler& sampler) {
    LMX_ASSERT(m_encoder, "bindSampler must be called between beginRenderPass and endRenderPass");
    // Samplers use ResourceID but need no residency registration because they are not allocations.
    m_argumentTable->setSamplerState(static_cast<Metal4Sampler&>(sampler).handle()->gpuResourceID(),
                                     slot);
}

//======================================================================================================================
void Metal4CommandList::setUniforms(uint32_t slot, const void* data, uint64_t size) {
    LMX_ASSERT(m_encoder, "setUniforms must be called between beginRenderPass and endRenderPass");
    LMX_ASSERT(data != nullptr && size > 0, "setUniforms: data must be non-null and non-empty");
    const uint64_t offset = *m_uniformOffset;
    const uint64_t capacity = m_uniformRing->length();
    // Use subtraction after bounding size to avoid overflow in offset + size.
    LMX_ASSERT(size <= capacity, "setUniforms: upload is larger than the entire per-frame uniform "
                                 "ring -- grow kUniformRingBytes");
    LMX_ASSERT(offset <= capacity - size,
               "setUniforms: per-frame uniform ring exhausted -- grow kUniformRingBytes");

    // Record the ring range only during capture. The label getter may return an autoreleased
    // string, so the otherwise allocation-free upload path creates a pool only in this branch.
    if (debug::CaptureSchema::instance().recordingUploads()) {
        NS::SharedPtr<NS::AutoreleasePool> pool =
            NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
        const NS::String* ringLabel = m_uniformRing->label();
        const char* utf8 = ringLabel != nullptr ? ringLabel->utf8String() : nullptr;
        debug::CaptureSchema::instance().recordUniformUpload(
            {utf8 != nullptr ? utf8 : "", slot, offset, size});
    }

    std::memcpy(static_cast<uint8_t*>(m_uniformRing->contents()) + offset, data, size);
    m_argumentTable->setAddress(m_uniformRing->gpuAddress() + offset, slot);
    *m_uniformOffset = alignUp(offset + size, kUniformOffsetAlignment);
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
void Metal4CommandList::endRenderPass() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_encoder, "endRenderPass: no render pass is open on this command list");
    m_encoder->endEncoding();
    m_encoder.reset();
}

//======================================================================================================================
void Metal4CommandList::textureBarrier(Texture& texture, TextureUse from, TextureUse to) {
    (void)texture; // Metal 4 barriers are stage-scoped rather than resource-scoped.
    LMX_ASSERT(!m_encoder, "textureBarrier must be called between render passes, not inside one");
    // Reject barriers outside a frame so a pending edge cannot leak into the next frame.
    LMX_ASSERT(m_argumentTable != nullptr, "textureBarrier must be called inside a frame");
    LMX_ASSERT(from == TextureUse::RenderTarget && to == TextureUse::ShaderRead,
               "textureBarrier: only RenderTarget -> ShaderRead is implemented (grown per demand)");
    // The next consumer encoder emits the pending barrier.
    m_pendingBarrier = true;
}

//======================================================================================================================
void Metal4CommandList::resetForFrame(MTL4::ArgumentTable* argumentTable, MTL::Buffer* uniformRing,
                                      uint64_t* uniformOffset) {
    // Never retarget per-frame storage while an encoder can still reference the old slot.
    LMX_ASSERT(!m_encoder, "resetForFrame: a render pass is still open from the previous frame");
    LMX_ASSERT(argumentTable != nullptr, "resetForFrame: argument table must not be null");
    LMX_ASSERT(uniformRing != nullptr && uniformOffset != nullptr,
               "resetForFrame: uniform ring and its offset cursor must not be null");
    m_argumentTable = argumentTable;
    m_uniformRing = uniformRing;
    m_uniformOffset = uniformOffset;
}

//======================================================================================================================
void Metal4CommandList::endFrameReset() {
    // A pending barrier at commit is an unconsumed dependency edge, not disposable state.
    LMX_ASSERT(!m_pendingBarrier, "textureBarrier recorded but no later pass consumed it");
    // Clearing per-frame pointers makes use outside a frame detectable.
    m_argumentTable = nullptr;
    m_uniformRing = nullptr;
    m_uniformOffset = nullptr;
    m_pendingBarrier = false;
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
