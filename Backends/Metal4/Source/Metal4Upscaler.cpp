//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Upscaler.cpp
/// @brief Implements the upscaler entries of the Metal 4 backend through MetalFX: creation, the recorded upscale and the retire hook.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Upscaler.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/Upscalers.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4TextureDescriptor.h"
#include "Metal4Timestamps.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        /// The narrowest and shortest output MetalFX creates a scaler for: below it, its creation aborts on an internal texture
        /// of zero extent.
        constexpr uint32_t kMinOutputExtent = 4;

        //==============================================================================================================
        /// Returns the smallest content extent, at most `input`, whose scale to `output` texels is at most `maxScale`.
        uint32_t smallestContent(uint32_t input, uint32_t output, float maxScale)
        {
            auto smallest = static_cast<uint32_t>(std::ceil(static_cast<float>(output) / maxScale));
            // The division may round below the exact quotient; the scale of the result must not pass the maximum.
            while ((smallest < input) && (static_cast<float>(output) / static_cast<float>(smallest) > maxScale))
            {
                ++smallest;
            }
            return std::clamp(smallest, 1u, input);
        }

        //==============================================================================================================
        /// Returns the `Unsupported` error that names why MetalFX makes no scaler for `record`, with its extents and formats.
        Error refusal(const core::UpscalerRecord& record, std::string_view reason)
        {
            return Error{
                ErrorCategory::Unsupported,
                std::format(
                    "MetalFX cannot create upscaler \"{}\" of {}x{} to {}x{} with the Format enum values color {}, depth {}, motion {} and output {}: {}",
                    record.label,
                    record.input.width,
                    record.input.height,
                    record.output.width,
                    record.output.height,
                    std::to_underlying(record.color),
                    std::to_underlying(record.depth),
                    std::to_underlying(record.motion),
                    std::to_underlying(record.outputFormat),
                    reason
                )
            };
        }

        //==============================================================================================================
        /// Returns the process-wide mutex that makes MetalFX create one temporal scaler at a time.
        std::mutex& scalerCreation()
        {
            static std::mutex mutex;
            return mutex;
        }

        //==============================================================================================================
        /// Makes the temporal scaler of `record` on `impl`'s device, or returns null when MetalFX makes none. Its range of
        /// content scales runs from the whole input's, the smaller of the two axes', to the device's maximum, so every content
        /// extent the core accepts down to that maximum fits it; the caller has refused a ratio beyond it. The caller holds an
        /// autorelease pool.
        MTL4FX::TemporalScaler* newScaler(DeviceImpl& impl, const core::UpscalerRecord& record, float minScale, float maxScale)
        {
            MTLFX::TemporalScalerDescriptor* descriptor = MTLFX::TemporalScalerDescriptor::alloc()->init();
            descriptor->setInputWidth(record.input.width);
            descriptor->setInputHeight(record.input.height);
            descriptor->setOutputWidth(record.output.width);
            descriptor->setOutputHeight(record.output.height);
            descriptor->setColorTextureFormat(metal4::pixelFormat(record.color));
            descriptor->setDepthTextureFormat(metal4::pixelFormat(record.depth));
            descriptor->setMotionTextureFormat(metal4::pixelFormat(record.motion));
            descriptor->setOutputTextureFormat(metal4::pixelFormat(record.outputFormat));
            descriptor->setReactiveMaskTextureEnabled(true);
            descriptor->setReactiveMaskTextureFormat(MTL::PixelFormatR8Unorm);
            descriptor->setInputContentPropertiesEnabled(true);
            descriptor->setInputContentMinScale(minScale);
            descriptor->setInputContentMaxScale(maxScale);
            descriptor->setAutoExposureEnabled((record.flags & UpscalerFlags::AutoExposure) != UpscalerFlags::None);
            // Creation is synchronous (chapter 12): the scaler is ready when createUpscaler returns.
            descriptor->setRequiresSynchronousInitialization(true);
            // MetalFX fills a process-wide table of Metal's while it makes a scaler, without a lock: two creations at once, on
            // any devices, corrupt it. Taken before the pipeline cache's lock, and nothing else is taken inside it.
            const std::lock_guard oneAtATime(scalerCreation());
            // The scaler's pipelines compile through the device's compiler, so they record into the pipeline cache.
            const std::shared_lock compiling = impl.pipelineCache.compiling();
            MTL4FX::TemporalScaler* scaler = descriptor->newTemporalScaler(impl.device, impl.compiler);
            descriptor->release();
            return scaler;
        }

        //==============================================================================================================
        /// Makes the reactive mask of `record`: an `R8Unorm` Private texture of the input extent in the device residency set,
        /// zeroed through the stager. Returns the texture, or the error that left none. The caller holds an autorelease pool.
        Result<MTL::Texture*> newMask(DeviceImpl& impl, const core::UpscalerRecord& record)
        {
            const TextureDesc desc{ .format = Format::R8Unorm, .extent = { record.input.width, record.input.height, 1 }, .usage = TextureUsage::Sampled };
            MTL::TextureDescriptor* descriptor = metal4::textureDescriptor(desc, false);
            MTL::Texture* mask = impl.device->newTexture(descriptor);
            descriptor->release();
            if (mask == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::OutOfMemory, std::format("no device memory for the reactive mask of upscaler \"{}\"", record.label) }
                );
            }
            mask->setLabel(metal4::makeString(record.label + ".reactiveMask"));
            impl.residency.add(mask);
            core::TextureRecord shape;
            shape.format = Format::R8Unorm;
            shape.extent = desc.extent;
            const std::vector<std::byte> zeros(size_t{ record.input.width } * record.input.height);
            const std::array<TextureData, 1> initial{ TextureData{ .bytes = zeros, .bytesPerRow = record.input.width } };
            if (Result<void> uploaded = impl.stager.uploadTexture(impl, shape, initial, mask); !uploaded)
            {
                impl.residency.remove(mask);
                mask->release();
                return std::unexpected(std::move(uploaded.error()));
            }
            return mask;
        }

        //==============================================================================================================
        /// Releases what `state` holds, after its mask leaves the device residency set. The caller holds an autorelease pool.
        void releaseState(DeviceImpl& impl, metal4::UpscalerState& state)
        {
            if (state.mask != nullptr)
            {
                impl.residency.remove(state.mask);
                state.mask->release();
            }
            if (state.scaler != nullptr)
            {
                state.scaler->release();
            }
            if (state.fence != nullptr)
            {
                state.fence->release();
            }
            state = {};
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        std::span<const UpscalerKind> metalUpscalers(MTL::Device* device)
        {
            static constexpr std::array<UpscalerKind, 1> kMetalFX = { UpscalerKind::MetalFX };
            if (MTLFX::TemporalScalerDescriptor::supportsMetal4FX(device))
            {
                return kMetalFX;
            }
            return {};
        }

        //==============================================================================================================
        void retireUpscaler(void* context, core::UpscalerRecord& record)
        {
            auto* state = static_cast<UpscalerState*>(record.native);
            if (state == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            {
                const AutoreleasePool pool;
                releaseState(impl, *state);
            }
            delete state;
            record.native = nullptr;
            // The pool counts the scaler's release; the fence and the mask are the backend's own objects for it.
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, 2);
        }
    } // namespace metal4

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler(const UpscalerDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The descriptor is checked and the kind resolved first; a kind the device does not list never reaches MetalFX.
        Result<core::UpscalerRecord> prepared = core::prepareUpscaler(impl.core, desc);
        if (!prepared)
        {
            return std::unexpected(std::move(prepared.error()));
        }
        core::UpscalerRecord record = std::move(*prepared);
        const metal4::AutoreleasePool pool;
        // MetalFX's creation aborts on a scale beyond the device's maximum and on a tiny output, so both are refused first.
        const float minSupported = MTLFX::TemporalScalerDescriptor::supportedInputContentMinScale(impl.device);
        const float maxSupported = MTLFX::TemporalScalerDescriptor::supportedInputContentMaxScale(impl.device);
        const float scaleX = static_cast<float>(record.output.width) / static_cast<float>(record.input.width);
        const float scaleY = static_cast<float>(record.output.height) / static_cast<float>(record.input.height);
        if (std::max(scaleX, scaleY) > maxSupported)
        {
            return std::unexpected(refusal(record, std::format("the scale exceeds the device's maximum of {}", maxSupported)));
        }
        if (record.output.width < kMinOutputExtent || record.output.height < kMinOutputExtent)
        {
            return std::unexpected(refusal(record, std::format("the output is under {} texels in an axis", kMinOutputExtent)));
        }
        metal4::UpscalerState state;
        state.scaler = newScaler(impl, record, std::clamp(std::min(scaleX, scaleY), minSupported, maxSupported), maxSupported);
        if (state.scaler == nullptr)
        {
            // The factory has no error out-parameter: nil is all MetalFX reports.
            return std::unexpected(refusal(record, "MetalFX made no temporal scaler"));
        }
        state.scaler->setDepthReversed((record.flags & UpscalerFlags::ReversedDepth) != UpscalerFlags::None);
        state.fence = impl.device->newFence();
        if (state.fence == nullptr)
        {
            releaseState(impl, state);
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("the fence of upscaler \"{}\" was not created", record.label) });
        }
        state.fence->setLabel(metal4::makeString(record.label + ".fence"));
        state.scaler->setFence(state.fence);
        Result<MTL::Texture*> mask = newMask(impl, record);
        if (!mask)
        {
            releaseState(impl, state);
            return std::unexpected(std::move(mask.error()));
        }
        state.mask = *mask;
        state.input = record.input;
        state.minContent = { smallestContent(record.input.width, record.output.width, maxSupported),
                             smallestContent(record.input.height, record.output.height, maxSupported) };
        auto* payload = new metal4::UpscalerState(state);
        record.native = payload;
        const Upscaler upscaler = impl.core.pool<Upscaler>().allocate(std::move(record));
        if (upscaler.bits == 0)
        {
            releaseState(impl, *payload);
            delete payload;
            return std::unexpected(core::tableFull("upscaler", impl.core.pool<Upscaler>().capacity()));
        }
        // The pool counts the scaler; the fence and the mask are the backend's own objects made for it.
        core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, 2);
        return upscaler;
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }

    //==================================================================================================================
    void CommandList::upscale(Upscaler upscaler, const UpscaleParams& params)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::upscale(list, upscaler, params);
        // The payloads are read without a lock (ADR 0020): the upscaler and the textures stay live until the frame retires.
        auto* state = static_cast<metal4::UpscalerState*>(list.device->pool<Upscaler>().native(upscaler));
        const auto& textures = list.device->pool<Texture>();
        // MetalFX encodes its own encoders on the command buffer, which takes them only between encoders.
        impl.encoders.close(list);
        metal4::ListState& native = impl.state;
        // The core recorded the upscale's scope with its label, or the default. The string is made with alloc and init, so it
        // is not autoreleased and no pool is opened; it labels the carrier while it is the innermost label.
        native.scratch.assign(list.scopeLog.back().label);
        NS::String* label = NS::String::alloc()->init(native.scratch.c_str(), NS::UTF8StringEncoding);
        if (label == nullptr)
        {
            label = NS::String::alloc()->init("rojorhi.upscale.unnamed", NS::UTF8StringEncoding);
        }
        native.commandBuffer->pushDebugGroup(label);
        core::noteNativeCall(list);
        native.scopeLabels.push_back(label);
        // The begin timestamp precedes the carrier, and the end one follows MetalFX's encoders, so the upscale's duration
        // takes in all of them.
        metal4::writeBetweenEncoders(impl, metal4::claimPair(impl));
        impl.encoders.carry(list, native, state->fence);
        MTL4FX::TemporalScaler* scaler = state->scaler;
        scaler->setColorTexture(static_cast<MTL::Texture*>(textures.native(params.color)));
        core::noteNativeCall(list);
        scaler->setDepthTexture(static_cast<MTL::Texture*>(textures.native(params.depth)));
        core::noteNativeCall(list);
        scaler->setMotionTexture(static_cast<MTL::Texture*>(textures.native(params.motion)));
        core::noteNativeCall(list);
        scaler->setOutputTexture(static_cast<MTL::Texture*>(textures.native(params.output)));
        core::noteNativeCall(list);
        // A null exposure reads slot zero, which is null, and binds nil.
        scaler->setExposureTexture(static_cast<MTL::Texture*>(textures.native(params.exposure)));
        core::noteNativeCall(list);
        scaler->setReactiveMaskTexture(params.reactive.bits == 0 ? state->mask : static_cast<MTL::Texture*>(textures.native(params.reactive)));
        core::noteNativeCall(list);
        // MetalFX takes no content whose scale to the output passes the device's maximum, which the core allows down to one
        // texel: such a content is raised to the smallest the scaler takes.
        const bool wholeInput = params.inputContent.width == 0 && params.inputContent.height == 0;
        scaler->setInputContentWidth(wholeInput ? state->input.width : std::max(params.inputContent.width, state->minContent.width));
        core::noteNativeCall(list);
        scaler->setInputContentHeight(wholeInput ? state->input.height : std::max(params.inputContent.height, state->minContent.height));
        core::noteNativeCall(list);
        scaler->setJitterOffsetX(params.jitterX);
        core::noteNativeCall(list);
        scaler->setJitterOffsetY(params.jitterY);
        core::noteNativeCall(list);
        scaler->setMotionVectorScaleX(params.motionScaleX);
        core::noteNativeCall(list);
        scaler->setMotionVectorScaleY(params.motionScaleY);
        core::noteNativeCall(list);
        scaler->setPreExposure(params.preExposure);
        core::noteNativeCall(list);
        scaler->setReset(params.reset);
        core::noteNativeCall(list);
        // The temporal scaler has no sharpening pass, so params.sharpness has no effect (chapter 12).
        {
            // MetalFX makes its own encoders, and on first use its pipelines, as autoreleased objects.
            const metal4::AutoreleasePool pool;
            scaler->encodeToCommandBuffer(native.commandBuffer);
        }
        core::noteNativeCall(list);
        metal4::writeBetweenEncoders(impl, metal4::popPairEnd(impl));
        native.commandBuffer->popDebugGroup();
        core::noteNativeCall(list);
        native.scopeLabels.pop_back();
        label->release();
        // The first compute and the first render encoder the list opens wait for MetalFX's encoders, which update the fence
        // when they finish.
        metal4::BarrierState& barriers = impl.encoders.barriers();
        barriers.owedFence = state->fence;
        barriers.owedFenceStages = metal4::kComputeStages | metal4::kRenderStages;
    }
} // namespace rojoRHI
