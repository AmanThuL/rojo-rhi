//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TemporalScaler.cpp
/// @brief Creates and encodes MetalFX temporal reconstruction with explicit fence handoff.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4TemporalScaler.h"

#include "Metal4CommandList.h"
#include "Metal4Device.h"
#include "Metal4DevicePrivate.h"
#include "Metal4Resources.h"
#include "RHI/Validate.h"

#include <array>
#include <utility>

namespace lmx::rhi::metal4 {
using namespace device_detail;

//======================================================================================================================
Metal4TemporalScalerState::~Metal4TemporalScalerState() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    scaler.reset();
    fence.reset();
    privateOutput.reset();
}

//======================================================================================================================
Result<std::unique_ptr<TemporalScaler>>
Metal4Device::createTemporalScaler(const TemporalScalerDesc& desc) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    if (!m_capabilities.temporalScaler.available) {
        return fail(ErrorCode::DeviceUnsupported, "device offers no temporal scaler");
    }
    const auto valid = validate(desc, m_capabilities.temporalScaler);
    LMX_ASSERT(valid.has_value(), valid.error().message);
    auto descriptor = NS::TransferPtr(MTLFX::TemporalScalerDescriptor::alloc()->init());
    descriptor->setInputWidth(desc.inputWidth);
    descriptor->setInputHeight(desc.inputHeight);
    descriptor->setOutputWidth(desc.outputWidth);
    descriptor->setOutputHeight(desc.outputHeight);
    descriptor->setColorTextureFormat(toMTL(desc.colorFormat));
    descriptor->setDepthTextureFormat(toMTL(desc.depthFormat));
    descriptor->setMotionTextureFormat(toMTL(desc.motionFormat));
    descriptor->setReactiveMaskTextureFormat(toMTL(desc.reactiveFormat));
    descriptor->setOutputTextureFormat(toMTL(desc.outputFormat));
    descriptor->setInputContentPropertiesEnabled(true);
    descriptor->setInputContentMinScale(1.0f / desc.maxInputScale);
    descriptor->setInputContentMaxScale(1.0f / desc.minInputScale);
    descriptor->setReactiveMaskTextureEnabled(true);
    descriptor->setAutoExposureEnabled(false);
    descriptor->setRequiresSynchronousInitialization(true);
    auto state = std::make_shared<Metal4TemporalScalerState>();
    state->scaler =
        NS::TransferPtr(descriptor->newTemporalScaler(m_device.get(), m_compiler.get()));
    if (!state->scaler) {
        // This factory has no NSError out-parameter; nil is all the vendor exposes.
        return fail(ErrorCode::ResourceCreationFailed,
                    "MetalFX declined temporal scaler creation (no vendor error detail available)");
    }
    state->desc = desc;
    state->label = resolveLabel(desc.label, "lmx.temporal.scaler");
    state->desc.label = state->label;
    state->fence = NS::TransferPtr(m_device->newFence());
    if (!state->fence) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "could not create the temporal scaler handoff fence");
    }
    // The scaler has no public label property; its fence and encode debug group carry the label.
    state->fence->setLabel(makeString(state->label + ".handoff").get());
    state->scaler->setFence(state->fence.get());
    const auto inputUsage = MTL::TextureUsageShaderRead;
    const auto outputUsage =
        MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget;
    LMX_ASSERT((state->scaler->colorTextureUsage() & inputUsage) ==
                       state->scaler->colorTextureUsage() &&
                   (state->scaler->depthTextureUsage() & inputUsage) ==
                       state->scaler->depthTextureUsage() &&
                   (state->scaler->motionTextureUsage() & inputUsage) ==
                       state->scaler->motionTextureUsage() &&
                   (state->scaler->reactiveTextureUsage() & inputUsage) ==
                       state->scaler->reactiveTextureUsage(),
               "MetalFX requires input usage the temporal contract cannot grant");
    LMX_ASSERT((state->scaler->outputTextureUsage() & outputUsage) ==
                   state->scaler->outputTextureUsage(),
               "MetalFX requires output usage the temporal contract cannot grant");
    // MetalFX requires private output storage. CPU-readable RHI outputs keep their contract by
    // receiving a copy from this resident private scratch inside the same external operation.
    auto output = createTexture({.width = desc.outputWidth,
                                 .height = desc.outputHeight,
                                 .format = desc.outputFormat,
                                 .renderTarget = true,
                                 .sampled = true,
                                 .storageWrite = true,
                                 .label = state->label + ".privateOutput"},
                                {});
    if (!output) {
        return std::unexpected(output.error());
    }
    state->privateOutput = std::move(*output);
    return std::make_unique<Metal4TemporalScaler>(std::move(state));
}

//======================================================================================================================
void Metal4CommandList::temporalScale(TemporalScaler& scaler, const TemporalScaleParams& params) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    LMX_ASSERT(m_argumentTable != nullptr && m_temporalScalers != nullptr,
               "temporalScale must be called inside a frame");
    LMX_ASSERT(!inPass(), "temporalScale must be called between passes");
    const auto& state = static_cast<Metal4TemporalScaler&>(scaler).state();
    const auto valid = validateTemporalScale(state->desc, params);
    LMX_ASSERT(valid.has_value(), valid.error().message);
    auto* native = state->scaler.get();
    const std::array<Texture*, 5> inputs{params.color, params.depth, params.motion, params.reactive,
                                         params.exposure};
    for (Texture* input : inputs) {
        LMX_ASSERT((static_cast<Metal4Texture*>(input)->handle()->usage() &
                    MTL::TextureUsageShaderRead) != 0,
                   "temporalScale inputs require sampled texture usage");
    }
    auto* output = static_cast<Metal4Texture*>(params.output)->handle();
    LMX_ASSERT((output->usage() & native->outputTextureUsage()) == native->outputTextureUsage(),
               "temporalScale output usage must include the scaler's required bits");
    const bool copyOutput = output->storageMode() != MTL::StorageModePrivate;
    auto* privateOutput = static_cast<Metal4Texture*>(state->privateOutput.get())->handle();
    native->setColorTexture(static_cast<Metal4Texture*>(params.color)->handle());
    native->setDepthTexture(static_cast<Metal4Texture*>(params.depth)->handle());
    native->setMotionTexture(static_cast<Metal4Texture*>(params.motion)->handle());
    native->setReactiveMaskTexture(static_cast<Metal4Texture*>(params.reactive)->handle());
    native->setExposureTexture(static_cast<Metal4Texture*>(params.exposure)->handle());
    native->setOutputTexture(copyOutput ? privateOutput : output);
    native->setInputContentWidth(params.inputContentWidth);
    native->setInputContentHeight(params.inputContentHeight);
    native->setJitterOffsetX(params.jitterOffsetX);
    native->setJitterOffsetY(params.jitterOffsetY);
    native->setMotionVectorScaleX(params.motionScaleX);
    native->setMotionVectorScaleY(params.motionScaleY);
    native->setPreExposure(params.preExposure);
    native->setReset(params.reset);
    native->setDepthReversed(params.reversedDepth);
    m_temporalScalers->push_back(state);
    const auto passLabel = resolveLabel(params.label, "lmx.pass.temporal.scaler");
    beginTimedPass(passLabel);
    m_commandBuffer->pushDebugGroup(makeString(passLabel).get());
    m_commandBuffer->pushDebugGroup(makeString(state->label).get());
    auto carrier = NS::RetainPtr(m_commandBuffer->computeCommandEncoder());
    LMX_ASSERT(carrier, "temporalScale could not create the fence carrier encoder");
    carrier->setLabel(makeString(std::string(passLabel) + ".handoff").get());
    constexpr auto carrierStages = MTL::StageDispatch | MTL::StageBlit;
    if (m_pendingTemporalFence != nullptr) {
        carrier->waitForFence(m_pendingTemporalFence, carrierStages);
        m_pendingTemporalFence = nullptr;
    }
    // Queue-stage visibility reaches all earlier producers, including scratch copies from prior
    // frames. The fence bridges that visibility into the vendor's opaque encoders.
    carrier->barrierAfterQueueStages(MTL::StageAll, MTL::StageAll,
                                     m_pendingBarrierVisibility | MTL4::VisibilityOptionDevice);
    m_pendingBarrierStages = MTL::Stages{};
    m_pendingBarrierVisibility = MTL4::VisibilityOptions{};
    carrier->updateFence(state->fence.get(), carrierStages);
    carrier->endEncoding();
    native->encodeToCommandBuffer(m_commandBuffer);
    m_pendingTemporalFence = state->fence.get();
    if (copyOutput) {
        auto copy = NS::RetainPtr(m_commandBuffer->computeCommandEncoder());
        LMX_ASSERT(copy, "temporalScale could not create the output copy encoder");
        copy->setLabel(makeString(std::string(passLabel) + ".outputCopy").get());
        emitPendingBarrier(copy.get(), carrierStages);
        copy->barrierAfterQueueStages(MTL::StageAll, carrierStages, MTL4::VisibilityOptionDevice);
        copy->copyFromTexture(privateOutput, 0, 0, MTL::Origin::Make(0, 0, 0),
                              MTL::Size::Make(state->desc.outputWidth, state->desc.outputHeight, 1),
                              output, 0, 0, MTL::Origin::Make(0, 0, 0));
        copy->endEncoding();
    }
    m_commandBuffer->popDebugGroup();
    m_commandBuffer->popDebugGroup();
    endTimedPass();
}

} // namespace lmx::rhi::metal4
