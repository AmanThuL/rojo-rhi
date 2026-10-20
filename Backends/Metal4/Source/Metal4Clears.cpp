//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Clears.cpp
/// @brief Implements `clearTexture` for the Metal 4 backend: render encoders with clear load actions, or the clear kernels for textures made for storage only.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Metal4Common.h"
#include "Metal4Encoders.h"
#include "Metal4Formats.h"
#include "Metal4Kernels.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rojoRHI {
    namespace {
        /// The arena bytes a clear dispatch's parameters take.
        constexpr uint32_t kClearParamsBytes = sizeof(metal4::ClearParams);

        /// The alignment of a clear dispatch's parameters: that of the `float4` that leads them.
        constexpr uint32_t kClearParamsAlignment = 16;

        /// The mips and layers a clear covers, with `kAllMips` and `kAllLayers` resolved.
        struct Subresources
        {
            uint32_t baseMip = 0;   ///< The first mip.
            uint32_t mips = 0;      ///< How many mips.
            uint32_t baseLayer = 0; ///< The first layer, counting faces for cube types.
            uint32_t layers = 0;    ///< How many layers.
        };

        //==============================================================================================================
        /// Returns the subresources `range` names in `texture`, read from the native texture's getters rather than from the
        /// record, which is behind a lock. Metal counts cubes in a cube type's array length, RojoRHI counts faces.
        Subresources resolve(const MTL::Texture* texture, const SubresourceRange& range)
        {
            const auto mips = static_cast<uint32_t>(texture->mipmapLevelCount());
            auto layers = static_cast<uint32_t>(texture->arrayLength());
            const MTL::TextureType type = texture->textureType();
            if (type == MTL::TextureTypeCube || type == MTL::TextureTypeCubeArray)
            {
                layers *= 6;
            }
            return Subresources{
                .baseMip = range.baseMip,
                .mips = range.mipCount == kAllMips ? mips - range.baseMip : range.mipCount,
                .baseLayer = range.baseLayer,
                .layers = range.layerCount == kAllLayers ? layers - range.baseLayer : range.layerCount,
            };
        }

        //==============================================================================================================
        /// Points `attachment` at mip `mip` and slice `layer` (depth plane `plane` of a 3D texture) of `texture`, cleared when
        /// the pass loads it and stored when it ends.
        void clearAttachment(MTL::RenderPassAttachmentDescriptor* attachment, const MTL::Texture* texture, uint32_t mip, uint32_t layer, uint32_t plane)
        {
            attachment->setTexture(texture);
            attachment->setLevel(mip);
            attachment->setSlice(layer);
            attachment->setDepthPlane(plane);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
        }

        //==============================================================================================================
        /// Clears `subresources` of `texture`, a render target or depth-stencil texture, with one render encoder per mip and
        /// layer (per depth plane of each mip for a 3D texture), each running only its clear load actions.
        void clearByLoadActions(CommandListImpl& impl, const MTL::Texture* texture, const Subresources& subresources, const ClearValue& value)
        {
            core::CommandListCore& list = impl.core;
            // A list has one open encoder, so the compute encoder ends before the render encoders.
            impl.encoders.close(list);
            const Format format = metal4::formatOf(texture->pixelFormat());
            const bool depth = core::isDepthFormat(format);
            const bool stencil = core::hasStencilPlane(format);
            const bool volume = texture->textureType() == MTL::TextureType3D;
            const MTL::ClearColor color(value.color.r, value.color.g, value.color.b, value.color.a);
            // The encoder factory autoreleases, and so may the attachment getters: one pool covers the whole clear.
            const metal4::AutoreleasePool pool;
            for (uint32_t mip = subresources.baseMip; mip < subresources.baseMip + subresources.mips; ++mip)
            {
                const uint32_t planes = volume ? std::max(static_cast<uint32_t>(texture->depth()) >> mip, 1u) : 1u;
                for (uint32_t layer = subresources.baseLayer; layer < subresources.baseLayer + subresources.layers; ++layer)
                {
                    for (uint32_t plane = 0; plane < planes; ++plane)
                    {
                        MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
                        if (depth)
                        {
                            MTL::RenderPassDepthAttachmentDescriptor* attachment = descriptor->depthAttachment();
                            clearAttachment(attachment, texture, mip, layer, 0);
                            attachment->setClearDepth(value.depth);
                            if (stencil)
                            {
                                MTL::RenderPassStencilAttachmentDescriptor* stencilAttachment = descriptor->stencilAttachment();
                                clearAttachment(stencilAttachment, texture, mip, layer, 0);
                                stencilAttachment->setClearStencil(value.stencil);
                            }
                        }
                        else
                        {
                            // A multisample attachment clears every sample by its load action.
                            MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
                            clearAttachment(attachment, texture, mip, layer, plane);
                            attachment->setClearColor(color);
                        }
                        impl.encoders.openRender(list, impl.state, descriptor);
                        impl.encoders.close(list);
                        descriptor->release();
                    }
                }
            }
        }

        //==============================================================================================================
        /// Clears `subresources` of `texture`, a texture made for storage only, with one dispatch of the clear kernel of its type
        /// per mip, on the list's compute encoder.
        void clearByKernel(CommandListImpl& impl, const MTL::Texture* texture, const Subresources& subresources, const ClearValue& value)
        {
            core::CommandListCore& list = impl.core;
            // The clear kernel is a dispatch, so a barrier held back for the dispatch stage goes before it.
            MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state, MTL::StageDispatch);
            // The retire hooks' context is the device that owns the core, and the kernels are fixed from its creation to its
            // destruction.
            core::DeviceCore& device = *list.device;
            const metal4::Kernels& kernels = static_cast<const DeviceImpl*>(device.retireHooks().context)->kernels;
            const MTL::TextureType type = texture->textureType();
            const MTL::ComputePipelineState* pipeline = kernels.clear[static_cast<size_t>(metal4::clearKernelOf(type))];
            const bool volume = type == MTL::TextureType3D;
            // The encoder takes the kernel table's bindings at each dispatch, so the texture is bound once for every mip.
            impl.state.kernelTable->setTexture(texture->gpuResourceID(), 0);
            core::noteNativeCall(list);
            for (uint32_t mip = subresources.baseMip; mip < subresources.baseMip + subresources.mips; ++mip)
            {
                const uint32_t width = std::max(static_cast<uint32_t>(texture->width()) >> mip, 1u);
                const uint32_t height = std::max(static_cast<uint32_t>(texture->height()) >> mip, 1u);
                // A dispatch covers every layer of the range, or every depth plane of a 3D texture's mip.
                const uint32_t layers = volume ? std::max(static_cast<uint32_t>(texture->depth()) >> mip, 1u) : subresources.layers;
                const metal4::ClearParams params{
                    .color = { value.color.r, value.color.g, value.color.b, value.color.a },
                    .baseLayer = volume ? 0 : subresources.baseLayer,
                    .mip = mip,
                    .width = width,
                    .height = height,
                    .layers = layers,
                    .reserved = {},
                };
                // The parameters live in the list's arena page, which only a claim locks; they count as no frame data upload.
                const core::ArenaAllocation place = core::bumpArenaPage(device, list, kClearParamsBytes, kClearParamsAlignment, device.arenaSource(), &device);
                std::memcpy(place.memory, &params, sizeof(params));
                metal4::bindKernel(list, impl.state, impl.encoders, pipeline);
                impl.state.kernelTable->setAddress(place.address, 0);
                core::noteNativeCall(list);
                const MTL::Size groups(
                    (width + metal4::kClearGroupEdge - 1) / metal4::kClearGroupEdge, (height + metal4::kClearGroupEdge - 1) / metal4::kClearGroupEdge, layers
                );
                metal4::dispatchKernel(list, impl.state, encoder, groups, MTL::Size(metal4::kClearGroupEdge, metal4::kClearGroupEdge, 1));
            }
            impl.encoders.markWork();
        }
    } // namespace

    //==================================================================================================================
    void CommandList::clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::clearTexture(list, texture, range, value);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020).
        const auto* native = static_cast<const MTL::Texture*>(list.device->pool<Texture>().native(texture));
        const Subresources subresources = resolve(native, range);
        // A render target or depth-stencil texture clears by load actions; a Compute list clears only textures made for
        // storage alone (VU-rojo-cmd-0061), which take the kernel.
        if ((native->usage() & MTL::TextureUsageRenderTarget) != 0)
        {
            clearByLoadActions(impl, native, subresources, value);
        }
        else
        {
            clearByKernel(impl, native, subresources, value);
        }
    }
} // namespace rojoRHI
