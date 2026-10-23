//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Views.cpp
/// @brief Implements the view, sampler and reserved-range entries of the Metal 4 backend and the hooks of their slots.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Views.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/DeviceCore.h"
#include "Core/Label.h"
#include "Core/Ranges.h"
#include "Core/Records.h"
#include "Core/Samplers.h"
#include "Core/Textures.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4PipelineState.h"
#include "Metal4TextureDescriptor.h"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Logs that Metal refused to create `what` and aborts, in every build: chapter 04 makes a refused write fatal.
        [[noreturn]] void refused(std::string_view what)
        {
            ROJORHI_LOG_FATAL("Metal refused to create {}", what);
            std::abort();
        }

        //==============================================================================================================
        /// Stores `id` at entry `slot` of `table`, a Shared buffer of `MTL::ResourceID`s.
        void writeId(MTL::Buffer* table, uint32_t slot, MTL::ResourceID id)
        {
            static_cast<MTL::ResourceID*>(table->contents())[slot] = id;
        }

        //==============================================================================================================
        /// Returns the view of `texture` that `desc`, which the core accepted, describes (+1): the texture itself, retained,
        /// when the view has its type, pixel format and whole mip and layer range, else a new texture view; null when Metal
        /// refuses. The caller holds an autorelease pool.
        MTL::Texture* newView(const core::TextureRecord& texture, const TextureViewDesc& desc)
        {
            auto* parent = static_cast<MTL::Texture*>(texture.native);
            const SubresourceRange& range = desc.range;
            const uint32_t mips = range.mipCount == kAllMips ? texture.mips - range.baseMip : range.mipCount;
            const uint32_t layers = range.layerCount == kAllLayers ? texture.layers - range.baseLayer : range.layerCount;
            // Only D32FloatS8 has a stencil plane to view, and Metal samples it through an X32_Stencil8 view.
            const MTL::PixelFormat format = desc.aspect == TextureAspect::Stencil
                                                ? MTL::PixelFormatX32_Stencil8
                                                : metal4::pixelFormat(desc.format == Format::Undefined ? texture.format : desc.format);
            const MTL::TextureType type = metal4::textureType(desc.type, texture.samples);
            const bool whole = range.baseMip == 0 && mips == texture.mips && range.baseLayer == 0 && layers == texture.layers;
            if (whole && format == metal4::pixelFormat(texture.format) && type == metal4::textureType(texture.type, texture.samples))
            {
                parent->retain();
                return parent;
            }
            // Slices of cube types count faces, as RojoRHI's layers do.
            MTL::Texture* view = parent->newTextureView(format, type, NS::Range::Make(range.baseMip, mips), NS::Range::Make(range.baseLayer, layers));
            if (view != nullptr)
            {
                view->setLabel(metal4::makeString(std::format("{}.view", texture.label)));
            }
            return view;
        }

        //==============================================================================================================
        /// Returns the sampler state of `desc`, which the core accepted (+1), or null when Metal refuses. The caller holds an
        /// autorelease pool.
        MTL::SamplerState* newSampler(DeviceImpl& impl, const SamplerDesc& desc)
        {
            MTL::SamplerDescriptor* descriptor = metal4::samplerDescriptor(desc, core::resolveLabel(desc.label, "rojorhi.sampler.unnamed"));
            MTL::SamplerState* sampler = impl.device->newSamplerState(descriptor);
            descriptor->release();
            return sampler;
        }

        //==============================================================================================================
        /// Returns the Metal minification and magnification filter of `filter`.
        MTL::SamplerMinMagFilter minMagFilter(SamplerFilter filter)
        {
            return filter == SamplerFilter::Nearest ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
        }

        //==============================================================================================================
        /// Returns the Metal mip filter of `filter`.
        MTL::SamplerMipFilter mipFilter(SamplerFilter filter)
        {
            return filter == SamplerFilter::Nearest ? MTL::SamplerMipFilterNearest : MTL::SamplerMipFilterLinear;
        }

        //==============================================================================================================
        /// Returns the Metal address mode of `address`.
        MTL::SamplerAddressMode addressMode(SamplerAddress address)
        {
            switch (address)
            {
            case SamplerAddress::Repeat: return MTL::SamplerAddressModeRepeat;
            case SamplerAddress::MirrorRepeat: return MTL::SamplerAddressModeMirrorRepeat;
            case SamplerAddress::ClampToEdge: return MTL::SamplerAddressModeClampToEdge;
            }
            return MTL::SamplerAddressModeRepeat;
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        MTL::SamplerDescriptor* samplerDescriptor(const SamplerDesc& desc, std::string_view label)
        {
            MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
            descriptor->setMinFilter(minMagFilter(desc.filter));
            descriptor->setMagFilter(minMagFilter(desc.filter));
            descriptor->setMipFilter(mipFilter(desc.mipFilter));
            descriptor->setSAddressMode(addressMode(desc.address));
            descriptor->setTAddressMode(addressMode(desc.address));
            descriptor->setRAddressMode(addressMode(desc.address));
            descriptor->setMaxAnisotropy(desc.maxAnisotropy);
            if (desc.compareEnable)
            {
                descriptor->setCompareFunction(compareFunction(desc.compare));
            }
            descriptor->setSupportArgumentBuffers(true);
            descriptor->setLabel(makeString(label));
            return descriptor;
        }

        //==============================================================================================================
        void retireView([[maybe_unused]] void* context, void* native)
        {
            if (native == nullptr)
            {
                return;
            }
            const AutoreleasePool pool;
            static_cast<MTL::Texture*>(native)->release();
        }

        //==============================================================================================================
        void retireSampler([[maybe_unused]] void* context, void* native)
        {
            if (native == nullptr)
            {
                return;
            }
            const AutoreleasePool pool;
            static_cast<MTL::SamplerState*>(native)->release();
        }
    } // namespace metal4

    //==================================================================================================================
    Result<ViewIndex> Device::createView(Texture texture, const TextureViewDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::TextureRecord& record = core::checkView(impl.core, texture, desc);
        const metal4::AutoreleasePool pool;
        MTL::Texture* view = newView(record, desc);
        if (view == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("Metal refused a view of texture \"{}\"", record.label) });
        }
        const MTL::ResourceID id = view->gpuResourceID();
        Result<ViewIndex> index = core::addView(impl.core, texture, view);
        if (!index)
        {
            view->release();
            return index;
        }
        writeId(impl.viewTable, std::to_underlying(*index), id);
        return index;
    }

    //==================================================================================================================
    void Device::writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::TextureRecord& record = core::checkViewWrite(impl.core, range, index, texture, desc);
        const metal4::AutoreleasePool pool;
        MTL::Texture* view = newView(record, desc);
        if (view == nullptr)
        {
            refused(std::format("a view of texture \"{}\" for slot {}", record.label, range.base + index));
        }
        const MTL::ResourceID id = view->gpuResourceID();
        if (!core::placeView(impl.core, range, index, texture, view))
        {
            // A racing release took the range after the check; the slot is not written.
            view->release();
            return;
        }
        writeId(impl.viewTable, range.base + index, id);
    }

    //==================================================================================================================
    void Device::destroy(ViewIndex view)
    {
        core::destroyView(DeviceImpl::of(*this).core, view);
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveViews(uint32_t count)
    {
        return core::reserveViews(DeviceImpl::of(*this).core, count);
    }

    //==================================================================================================================
    Result<SamplerIndex> Device::createSampler(const SamplerDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkSampler(impl.core, desc);
        const metal4::AutoreleasePool pool;
        MTL::SamplerState* sampler = newSampler(impl, desc);
        if (sampler == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "Metal refused a sampler state" });
        }
        const MTL::ResourceID id = sampler->gpuResourceID();
        Result<SamplerIndex> index = core::addSampler(impl.core, sampler);
        if (!index)
        {
            sampler->release();
            return index;
        }
        writeId(impl.samplerTable, std::to_underlying(*index), id);
        return index;
    }

    //==================================================================================================================
    void Device::writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkSamplerWrite(impl.core, range, index, desc);
        const metal4::AutoreleasePool pool;
        MTL::SamplerState* sampler = newSampler(impl, desc);
        if (sampler == nullptr)
        {
            refused(std::format("a sampler state for slot {}", range.base + index));
        }
        const MTL::ResourceID id = sampler->gpuResourceID();
        if (!core::placeSampler(impl.core, range, index, sampler))
        {
            // A racing release took the range after the check; the slot is not written.
            sampler->release();
            return;
        }
        writeId(impl.samplerTable, range.base + index, id);
    }

    //==================================================================================================================
    void Device::destroy(SamplerIndex sampler)
    {
        core::destroySampler(DeviceImpl::of(*this).core, sampler);
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveSamplers(uint32_t count)
    {
        return core::reserveSamplers(DeviceImpl::of(*this).core, count);
    }

    //==================================================================================================================
    void Device::release(IndexRange range)
    {
        core::releaseRange(DeviceImpl::of(*this).core, range);
    }
} // namespace rojoRHI
