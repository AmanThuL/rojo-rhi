//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Textures.cpp
/// @brief Implements the D3D12 texture entries, the render-target and depth-stencil views of a texture and its retire hook.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Textures.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Heaps.h"
#include "Core/Records.h"
#include "Core/Textures.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12Formats.h"
#include "D3D12Sparse.h"
#include "D3D12Stager.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns true when `usage` has every bit of `bit`.
        bool hasUsage(TextureUsage usage, TextureUsage bit)
        {
            return (usage & bit) == bit;
        }

        //==============================================================================================================
        /// Returns the depth slices of mip `mip` of a texture whose mip 0 has `depth` slices, never below one.
        uint32_t slicesOf(uint32_t depth, uint32_t mip)
        {
            return std::max(depth >> mip, 1u);
        }

        //==============================================================================================================
        /// Returns the resource dimension of a texture of `type`: a cube is a 2D array of six faces per cube.
        D3D12_RESOURCE_DIMENSION dimensionOf(TextureType type)
        {
            switch (type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray: return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            case TextureType::Tex3D: return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray:
            case TextureType::Cube:
            case TextureType::CubeArray: return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            }
            return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }

        //==============================================================================================================
        /// Returns the description of the resource of a texture of `desc`, which the core accepted, on `impl`'s device.
        D3D12_RESOURCE_DESC1 resourceDesc(const DeviceImpl& impl, const TextureDesc& desc)
        {
            D3D12_RESOURCE_DESC1 native = {};
            native.Dimension = dimensionOf(desc.type);
            native.Alignment = 0;
            native.Width = desc.extent.width;
            native.Height = desc.extent.height;
            // The limits keep both counts within 16 bits.
            native.DepthOrArraySize = static_cast<UINT16>(desc.type == TextureType::Tex3D ? desc.extent.depth : desc.layers);
            native.MipLevels = static_cast<UINT16>(desc.mips);
            native.Format = d3d12::resourceFormat(desc.format, desc.usage, impl.features.relaxedFormatCasting);
            native.SampleDesc.Count = desc.samples;
            native.SampleDesc.Quality = 0;
            native.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            native.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (hasUsage(desc.usage, TextureUsage::RenderTarget))
            {
                native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (hasUsage(desc.usage, TextureUsage::DepthStencil))
            {
                native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                // A depth texture no shader samples denies shader views, which lets the device compress it fully.
                if (!hasUsage(desc.usage, TextureUsage::Sampled))
                {
                    native.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
                }
            }
            if (hasUsage(desc.usage, TextureUsage::Storage))
            {
                native.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            return native;
        }

        //==============================================================================================================
        /// Returns the formats a texture of `desc` is created castable to on `impl`'s device: its format family when the
        /// device casts formats, else none, the resource then being typeless where a view may reinterpret it.
        std::span<const DXGI_FORMAT> castablesOf(const DeviceImpl& impl, const TextureDesc& desc)
        {
            if (!impl.features.relaxedFormatCasting)
            {
                return {};
            }
            return d3d12::castableFormats(desc.format);
        }

        //==============================================================================================================
        /// Returns the bytes and alignment the resource `native`, castable to `castable`, occupies in a heap on `impl`'s device.
        /// A description the runtime refuses is logged and aborts, in every build: the core accepted it.
        PlacementRequirements allocationOf(DeviceImpl& impl, const D3D12_RESOURCE_DESC1& native, std::span<const DXGI_FORMAT> castable)
        {
            const auto count = static_cast<UINT32>(castable.size());
            const DXGI_FORMAT* formats = castable.data();
            const D3D12_RESOURCE_ALLOCATION_INFO info =
                impl.device()->GetResourceAllocationInfo3(0, 1, &native, count == 0 ? nullptr : &count, count == 0 ? nullptr : &formats, nullptr);
            if (info.SizeInBytes == UINT64_MAX)
            {
                ROJORHI_LOG_FATAL("ID3D12Device12::GetResourceAllocationInfo3 refused a texture of format {}", static_cast<uint32_t>(native.Format));
                std::abort();
            }
            return PlacementRequirements{ info.SizeInBytes, info.Alignment };
        }

        //==============================================================================================================
        /// Returns the render-target view of `texture`'s subresource at `mip` and `slice`, an array layer or a depth slice.
        D3D12_RENDER_TARGET_VIEW_DESC renderTargetDesc(const core::TextureRecord& texture, uint32_t mip, uint32_t slice)
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = d3d12::viewFormat(texture.format, Format::Undefined, TextureAspect::Color, TextureUsage::RenderTarget);
            // A non-array view names a resource of one layer, an array view of one layer any other.
            const bool arrayed = texture.layers > 1;
            switch (texture.type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray:
                if (arrayed)
                {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                    desc.Texture1DArray.MipSlice = mip;
                    desc.Texture1DArray.FirstArraySlice = slice;
                    desc.Texture1DArray.ArraySize = 1;
                }
                else
                {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                    desc.Texture1D.MipSlice = mip;
                }
                break;
            case TextureType::Tex3D:
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                desc.Texture3D.MipSlice = mip;
                desc.Texture3D.FirstWSlice = slice;
                desc.Texture3D.WSize = 1;
                break;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray:
            case TextureType::Cube:
            case TextureType::CubeArray:
                if (texture.samples > 1)
                {
                    if (arrayed)
                    {
                        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                        desc.Texture2DMSArray.FirstArraySlice = slice;
                        desc.Texture2DMSArray.ArraySize = 1;
                    }
                    else
                    {
                        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                    }
                }
                else if (arrayed)
                {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                    desc.Texture2DArray.MipSlice = mip;
                    desc.Texture2DArray.FirstArraySlice = slice;
                    desc.Texture2DArray.ArraySize = 1;
                    desc.Texture2DArray.PlaneSlice = 0;
                }
                else
                {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    desc.Texture2D.MipSlice = mip;
                    desc.Texture2D.PlaneSlice = 0;
                }
                break;
            }
            return desc;
        }

        //==============================================================================================================
        /// Returns the depth-stencil view of `texture`'s subresource at `mip` and array layer `layer`.
        D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc(const core::TextureRecord& texture, uint32_t mip, uint32_t layer)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
            desc.Format = d3d12::dsvFormat(texture.format);
            desc.Flags = D3D12_DSV_FLAG_NONE;
            const bool arrayed = texture.layers > 1;
            const bool oneDimensional = texture.type == TextureType::Tex1D || texture.type == TextureType::Tex1DArray;
            if (oneDimensional && arrayed)
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                desc.Texture1DArray.MipSlice = mip;
                desc.Texture1DArray.FirstArraySlice = layer;
                desc.Texture1DArray.ArraySize = 1;
            }
            else if (oneDimensional)
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                desc.Texture1D.MipSlice = mip;
            }
            else if (texture.samples > 1 && arrayed)
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                desc.Texture2DMSArray.FirstArraySlice = layer;
                desc.Texture2DMSArray.ArraySize = 1;
            }
            else if (texture.samples > 1)
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
            else if (arrayed)
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipSlice = mip;
                desc.Texture2DArray.FirstArraySlice = layer;
                desc.Texture2DArray.ArraySize = 1;
            }
            else
            {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = mip;
            }
            return desc;
        }

        //==============================================================================================================
        /// Creates the CPU-only heap of `texture`'s render-target views, or depth-stencil views when `depth`, one per mip and
        /// layer, per mip and depth slice for a 3D texture, named `<label>.rtv` or `<label>.dsv`, writes every view of
        /// `resource` into it and records them in `payload`. The heap counts one `nativeCreateCalls` and each view one
        /// `descriptorWrites`.
        Result<void> createTargets(DeviceImpl& impl, ID3D12Resource* resource, const core::TextureRecord& texture, bool depth, d3d12::TexturePayload& payload)
        {
            const bool volume = texture.type == TextureType::Tex3D;
            const uint32_t layers = volume ? 1 : texture.layers;
            const uint32_t depthSlices = volume ? texture.extent.depth : 1;
            uint32_t count = 0;
            for (uint32_t mip = 0; mip < texture.mips; ++mip)
            {
                count += layers * slicesOf(depthSlices, mip);
            }
            const D3D12_DESCRIPTOR_HEAP_TYPE type = depth ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            const std::string label = std::format("{}.{}", texture.label, depth ? "dsv" : "rtv");
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = type;
            heapDesc.NumDescriptors = count;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            heapDesc.NodeMask = 0;
            d3d12::Com<ID3D12DescriptorHeap> heap;
            if (const HRESULT created = impl.device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.put())); FAILED(created))
            {
                return std::unexpected(d3d12::failure(impl, created, std::format("ID3D12Device::CreateDescriptorHeap for {} of {} descriptors", label, count)));
            }
            d3d12::setName(heap.get(), label);
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            const D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
            const UINT increment = impl.device()->GetDescriptorHandleIncrementSize(type);
            uint32_t index = 0;
            for (uint32_t mip = 0; mip < texture.mips; ++mip)
            {
                const uint32_t slices = layers * slicesOf(depthSlices, mip);
                for (uint32_t slice = 0; slice < slices; ++slice, ++index)
                {
                    const D3D12_CPU_DESCRIPTOR_HANDLE handle{ start.ptr + SIZE_T{ index } * increment };
                    if (depth)
                    {
                        const D3D12_DEPTH_STENCIL_VIEW_DESC view = depthStencilDesc(texture, mip, slice);
                        impl.device()->CreateDepthStencilView(resource, &view, handle);
                    }
                    else
                    {
                        const D3D12_RENDER_TARGET_VIEW_DESC view = renderTargetDesc(texture, mip, slice);
                        impl.device()->CreateRenderTargetView(resource, &view, handle);
                    }
                    core::DeviceCounts::add(impl.core.counts().descriptorWrites);
                }
            }
            payload.targets = std::move(heap);
            payload.targetCount = count;
            payload.targetStart = start;
            payload.targetIncrement = increment;
            payload.targetLayers = layers;
            payload.targetDepth = depthSlices;
            return {};
        }

        //==============================================================================================================
        /// Copies `initial`, one entry per subresource in the reference's order, into `resource`, created in
        /// `D3D12_BARRIER_LAYOUT_COPY_DEST`, and leaves every subresource in `D3D12_BARRIER_LAYOUT_SHADER_RESOURCE`.
        Result<void> uploadInitial(DeviceImpl& impl, ID3D12Resource* resource, std::span<const TextureData> initial)
        {
            // The reference orders entries by layer, then by mip within a layer (index mip + layer * mips), which is the D3D12
            // subresource index of a single-plane texture: MipSlice + ArraySlice * MipLevels. A 3D texture has one layer, and
            // the one format with two planes, D32FloatS8, takes no initial data.
            std::vector<d3d12::SubresourceData> subresources;
            subresources.reserve(initial.size());
            for (const TextureData& data : initial)
            {
                subresources.push_back(d3d12::SubresourceData{ .bytes = data.bytes, .bytesPerRow = data.bytesPerRow, .bytesPerImage = data.bytesPerImage });
            }
            return impl.stager.uploadTexture(impl, resource, subresources);
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload& payload, uint32_t mip, uint32_t layer)
        {
            ROJORHI_INTERNAL_ASSERT(
                payload.targetCount > 0 && layer < payload.targetLayers * slicesOf(payload.targetDepth, mip),
                "a target view of a layer or depth slice the mip does not have"
            );
            uint32_t index = layer;
            for (uint32_t earlier = 0; earlier < mip; ++earlier)
            {
                index += payload.targetLayers * slicesOf(payload.targetDepth, earlier);
            }
            ROJORHI_INTERNAL_ASSERT(index < payload.targetCount, "a target view outside the texture's render-target or depth-stencil views");
            return D3D12_CPU_DESCRIPTOR_HANDLE{ payload.targetStart.ptr + SIZE_T{ index } * payload.targetIncrement };
        }

        //==============================================================================================================
        void retireTexture(void* context, core::TextureRecord& record)
        {
            auto* payload = static_cast<TexturePayload*>(record.native);
            // A swapchain image borrows its payload from the swapchain, which releases the back buffer and its view itself.
            if (payload == nullptr || record.swapchainImage)
            {
                record.native = nullptr;
                return;
            }
            if (payload->targets)
            {
                // The target heap is an internal object, so its release counts as one native create or release call.
                payload->targets.reset();
                core::DeviceCounts::add(static_cast<DeviceImpl*>(context)->core.counts().nativeCreateCalls);
            }
            payload->resource->Release();
            delete payload;
            record.native = nullptr;
        }
    } // namespace d3d12

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const TextureDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkTextureRequirementsQuery(impl.core, desc);
        return allocationOf(impl, resourceDesc(impl, desc), castablesOf(impl, desc));
    }

    //==================================================================================================================
    Result<Texture> Device::createTexture(const TextureDesc& desc, std::span<const TextureData> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const bool placed = placement.heap.bits != 0;
        const D3D12_RESOURCE_DESC1 native = resourceDesc(impl, desc);
        const std::span<const DXGI_FORMAT> castable = castablesOf(impl, desc);
        PlacementRequirements requirements;
        if (placed)
        {
            // The runtime never sees a description the core rejects.
            core::checkTextureDesc(impl.core, desc);
            requirements = allocationOf(impl, native, castable);
        }
        core::TextureRecord record = core::prepareTexture(impl.core, desc, initial, placement, requirements);
        // A texture without data starts in COMMON, the one layout a copy queue accesses; its first barrier from Undefined
        // still names LAYOUT_UNDEFINED and discards, which is legal whatever the layout.
        const D3D12_BARRIER_LAYOUT layout = initial.empty() ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_COPY_DEST;
        const auto castableCount = static_cast<UINT32>(castable.size());
        const DXGI_FORMAT* castableList = castableCount == 0 ? nullptr : castable.data();
        d3d12::Com<ID3D12Resource> resource;
        if (placed)
        {
            auto* heap = static_cast<ID3D12Heap*>(core::checkedHeap(impl.core, placement.heap).native);
            if (const HRESULT created = impl.device()->CreatePlacedResource2(
                    heap, placement.offset, &native, layout, nullptr, castableCount, castableList, IID_PPV_ARGS(resource.put())
                );
                FAILED(created))
            {
                return std::unexpected(d3d12::failure(impl, created, std::format("ID3D12Device10::CreatePlacedResource2 for texture \"{}\"", record.label)));
            }
        }
        else if (hasUsage(desc.usage, TextureUsage::Sparse))
        {
            Result<d3d12::Com<ID3D12Resource>> reserved = d3d12::createReservedTexture(impl, desc, native, castable, record.label);
            if (!reserved)
            {
                return std::unexpected(std::move(reserved.error()));
            }
            resource = std::move(*reserved);
            // The shape is measured once, before the record is published, and never changes while the texture lives.
            record.tiles = d3d12::readTileShape(*impl.device(), *resource, desc.mips);
        }
        else
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (const HRESULT created = impl.device()->CreateCommittedResource3(
                    &heap, D3D12_HEAP_FLAG_NONE, &native, layout, nullptr, nullptr, castableCount, castableList, IID_PPV_ARGS(resource.put())
                );
                FAILED(created))
            {
                return std::unexpected(d3d12::failure(impl, created, std::format("ID3D12Device10::CreateCommittedResource3 for texture \"{}\"", record.label)));
            }
        }
        d3d12::setName(resource.get(), record.label);
        auto payload = std::make_unique<d3d12::TexturePayload>();
        core::DeviceCounts::add(impl.core.counts().cpuAllocations);
        payload->resourceFormat = native.Format;
        payload->format = record.format;
        payload->type = record.type;
        payload->usage = record.usage;
        payload->width = record.extent.width;
        payload->height = record.extent.height;
        payload->mips = record.mips;
        const bool volume = record.type == TextureType::Tex3D;
        payload->layers = volume ? 1 : record.layers;
        payload->depth = volume ? record.extent.depth : 1;
        payload->samples = record.samples;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        impl.device()->GetCopyableFootprints1(&native, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        payload->copyFormat = footprint.Footprint.Format;
        const bool renderTarget = hasUsage(desc.usage, TextureUsage::RenderTarget);
        if (renderTarget || hasUsage(desc.usage, TextureUsage::DepthStencil))
        {
            if (Result<void> made = createTargets(impl, resource.get(), record, !renderTarget, *payload); !made)
            {
                return std::unexpected(std::move(made.error()));
            }
        }
        if (!initial.empty())
        {
            if (Result<void> uploaded = uploadInitial(impl, resource.get(), initial); !uploaded)
            {
                return std::unexpected(std::move(uploaded.error()));
            }
        }
        payload->resource = resource.get();
        record.native = payload.get();
        Result<Texture> handle = core::addTexture(impl.core, std::move(record));
        if (handle)
        {
            // The record owns the payload and the resource's reference now; the retire hook releases both.
            (void)resource.detach();
            (void)payload.release();
        }
        return handle;
    }

    //==================================================================================================================
    void Device::destroy(Texture texture)
    {
        core::destroyTexture(DeviceImpl::of(*this).core, texture);
    }
} // namespace rojoRHI
