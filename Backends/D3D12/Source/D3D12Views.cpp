//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Views.cpp
/// @brief Implements the D3D12 view, sampler and reserved-range entries over the shader-visible heaps, and the view slots' hook.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Views.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Ranges.h"
#include "Core/Records.h"
#include "Core/Samplers.h"
#include "Core/Textures.h"
#include "D3D12Common.h"
#include "D3D12Descriptors.h"
#include "D3D12Device.h"
#include "D3D12Formats.h"
#include "D3D12Textures.h"

#include <cstdint>
#include <utility>

namespace rojoRHI {
    namespace {
        /// The faces of one cube.
        constexpr uint32_t kCubeFaces = 6;

        /// The mips and layers a view covers, its counts resolved against its texture.
        struct ViewExtent
        {
            uint32_t baseMip = 0;   ///< The first mip.
            uint32_t mips = 1;      ///< The mips covered.
            uint32_t baseLayer = 0; ///< The first layer, a face for cube types.
            uint32_t layers = 1;    ///< The layers covered.
        };

        //==============================================================================================================
        /// Returns the mips and layers of `texture` that `range`, which the core accepted, covers.
        ViewExtent extentOf(const core::TextureRecord& texture, const SubresourceRange& range)
        {
            ViewExtent extent;
            extent.baseMip = range.baseMip;
            extent.mips = range.mipCount == kAllMips ? texture.mips - range.baseMip : range.mipCount;
            extent.baseLayer = range.baseLayer;
            extent.layers = range.layerCount == kAllLayers ? texture.layers - range.baseLayer : range.layerCount;
            return extent;
        }

        //==============================================================================================================
        /// Returns true when the resource of `texture` has more than one array layer. A non-array view dimension names no
        /// layer, so a single-layer view of such a resource takes the array dimension with one layer.
        bool arrayed(const core::TextureRecord& texture)
        {
            return texture.type != TextureType::Tex3D && texture.layers > 1;
        }

        //==============================================================================================================
        /// Returns the shader-resource view of `texture` that `desc`, a `Sampled` view the core accepted, describes. Its
        /// dimension follows the view's type; a stencil view reads plane 1 of a depth-stencil format.
        D3D12_SHADER_RESOURCE_VIEW_DESC shaderResourceDesc(const core::TextureRecord& texture, const TextureViewDesc& desc)
        {
            const ViewExtent extent = extentOf(texture, desc.range);
            const UINT plane = desc.aspect == TextureAspect::Stencil ? 1u : 0u;
            D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
            view.Format = d3d12::viewFormat(texture.format, desc.format, desc.aspect, TextureUsage::Sampled);
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            switch (desc.type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray:
                if (desc.type == TextureType::Tex1D && !arrayed(texture))
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                    view.Texture1D.MostDetailedMip = extent.baseMip;
                    view.Texture1D.MipLevels = extent.mips;
                    view.Texture1D.ResourceMinLODClamp = 0.0f;
                }
                else
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                    view.Texture1DArray.MostDetailedMip = extent.baseMip;
                    view.Texture1DArray.MipLevels = extent.mips;
                    view.Texture1DArray.FirstArraySlice = extent.baseLayer;
                    view.Texture1DArray.ArraySize = extent.layers;
                    view.Texture1DArray.ResourceMinLODClamp = 0.0f;
                }
                break;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray:
                if (texture.samples > 1)
                {
                    if (desc.type == TextureType::Tex2D && !arrayed(texture))
                    {
                        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                    }
                    else
                    {
                        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                        view.Texture2DMSArray.FirstArraySlice = extent.baseLayer;
                        view.Texture2DMSArray.ArraySize = extent.layers;
                    }
                }
                else if (desc.type == TextureType::Tex2D && !arrayed(texture))
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MostDetailedMip = extent.baseMip;
                    view.Texture2D.MipLevels = extent.mips;
                    view.Texture2D.PlaneSlice = plane;
                    view.Texture2D.ResourceMinLODClamp = 0.0f;
                }
                else
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MostDetailedMip = extent.baseMip;
                    view.Texture2DArray.MipLevels = extent.mips;
                    view.Texture2DArray.FirstArraySlice = extent.baseLayer;
                    view.Texture2DArray.ArraySize = extent.layers;
                    view.Texture2DArray.PlaneSlice = plane;
                    view.Texture2DArray.ResourceMinLODClamp = 0.0f;
                }
                break;
            case TextureType::Tex3D:
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                view.Texture3D.MostDetailedMip = extent.baseMip;
                view.Texture3D.MipLevels = extent.mips;
                view.Texture3D.ResourceMinLODClamp = 0.0f;
                break;
            case TextureType::Cube:
            case TextureType::CubeArray:
                // A cube view names no first face, so a cube other than the first is a cube array of one cube.
                if (desc.type == TextureType::Cube && extent.baseLayer == 0)
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    view.TextureCube.MostDetailedMip = extent.baseMip;
                    view.TextureCube.MipLevels = extent.mips;
                    view.TextureCube.ResourceMinLODClamp = 0.0f;
                }
                else
                {
                    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                    view.TextureCubeArray.MostDetailedMip = extent.baseMip;
                    view.TextureCubeArray.MipLevels = extent.mips;
                    view.TextureCubeArray.First2DArrayFace = extent.baseLayer;
                    view.TextureCubeArray.NumCubes = extent.layers / kCubeFaces;
                    view.TextureCubeArray.ResourceMinLODClamp = 0.0f;
                }
                break;
            }
            return view;
        }

        //==============================================================================================================
        /// Returns the unordered-access view of `texture` that `desc`, a `Storage` view of one mip the core accepted,
        /// describes. A UAV has no cube dimension, so a cube view is the 2D array of its faces; an sRGB format is written
        /// through its linear partner.
        D3D12_UNORDERED_ACCESS_VIEW_DESC unorderedAccessDesc(const core::TextureRecord& texture, const TextureViewDesc& desc)
        {
            const ViewExtent extent = extentOf(texture, desc.range);
            D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
            view.Format = d3d12::viewFormat(texture.format, desc.format, desc.aspect, TextureUsage::Storage);
            switch (desc.type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray:
                if (desc.type == TextureType::Tex1D && !arrayed(texture))
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                    view.Texture1D.MipSlice = extent.baseMip;
                }
                else
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                    view.Texture1DArray.MipSlice = extent.baseMip;
                    view.Texture1DArray.FirstArraySlice = extent.baseLayer;
                    view.Texture1DArray.ArraySize = extent.layers;
                }
                break;
            case TextureType::Tex3D:
                view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                view.Texture3D.MipSlice = extent.baseMip;
                view.Texture3D.FirstWSlice = 0;
                view.Texture3D.WSize = core::mipExtent(texture.extent, extent.baseMip).depth;
                break;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray:
            case TextureType::Cube:
            case TextureType::CubeArray:
                if (desc.type == TextureType::Tex2D && !arrayed(texture))
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    view.Texture2D.MipSlice = extent.baseMip;
                    view.Texture2D.PlaneSlice = 0;
                }
                else
                {
                    view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.MipSlice = extent.baseMip;
                    view.Texture2DArray.FirstArraySlice = extent.baseLayer;
                    view.Texture2DArray.ArraySize = extent.layers;
                    view.Texture2DArray.PlaneSlice = 0;
                }
                break;
            }
            return view;
        }

        //==============================================================================================================
        /// Writes the view of `texture` that `desc`, which the core accepted, describes into view-heap slot `index` of `impl`.
        /// The slot's payload holds a reference to `resource`, so the descriptor never outlives the resource it names.
        void writeTextureView(DeviceImpl& impl, uint32_t index, ID3D12Resource* resource, const core::TextureRecord& texture, const TextureViewDesc& desc)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE handle = impl.descriptors.viewCpu(index);
            if (desc.usage == TextureUsage::Storage)
            {
                const D3D12_UNORDERED_ACCESS_VIEW_DESC view = unorderedAccessDesc(texture, desc);
                impl.device()->CreateUnorderedAccessView(resource, nullptr, &view, handle);
            }
            else
            {
                const D3D12_SHADER_RESOURCE_VIEW_DESC view = shaderResourceDesc(texture, desc);
                impl.device()->CreateShaderResourceView(resource, &view, handle);
            }
        }

        //==============================================================================================================
        /// Returns the D3D12 filter type of `filter`.
        D3D12_FILTER_TYPE filterType(SamplerFilter filter)
        {
            return filter == SamplerFilter::Nearest ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
        }

        //==============================================================================================================
        /// Returns the D3D12 address mode of `address`.
        D3D12_TEXTURE_ADDRESS_MODE addressMode(SamplerAddress address)
        {
            switch (address)
            {
            case SamplerAddress::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case SamplerAddress::MirrorRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case SamplerAddress::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }

        //==============================================================================================================
        /// Returns the D3D12 comparison function of `compare`.
        D3D12_COMPARISON_FUNC comparisonFunc(CompareOp compare)
        {
            switch (compare)
            {
            case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
            case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case CompareOp::LessOrEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
            return D3D12_COMPARISON_FUNC_NEVER;
        }

        //==============================================================================================================
        /// Returns the D3D12 sampler of `desc`, which the core accepted: the comparison filters when `compareEnable`, the
        /// anisotropic ones when `maxAnisotropy` is above 1, every mip level reachable and no LOD bias.
        D3D12_SAMPLER_DESC samplerDesc(const SamplerDesc& desc)
        {
            const D3D12_FILTER_REDUCTION_TYPE reduction = desc.compareEnable ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
            D3D12_SAMPLER_DESC sampler = {};
            // The core requires Linear filters with anisotropy (VU-rojo-texture-0030).
            sampler.Filter = desc.maxAnisotropy > 1
                                 ? D3D12_ENCODE_ANISOTROPIC_FILTER(reduction)
                                 : D3D12_ENCODE_BASIC_FILTER(filterType(desc.filter), filterType(desc.filter), filterType(desc.mipFilter), reduction);
            sampler.AddressU = addressMode(desc.address);
            sampler.AddressV = addressMode(desc.address);
            sampler.AddressW = addressMode(desc.address);
            sampler.MipLODBias = 0.0f;
            sampler.MaxAnisotropy = desc.maxAnisotropy;
            // A sampler that compares nothing ignores the function; NEVER is the conventional value there.
            sampler.ComparisonFunc = desc.compareEnable ? comparisonFunc(desc.compare) : D3D12_COMPARISON_FUNC_NEVER;
            sampler.BorderColor[0] = 0.0f;
            sampler.BorderColor[1] = 0.0f;
            sampler.BorderColor[2] = 0.0f;
            sampler.BorderColor[3] = 0.0f;
            sampler.MinLOD = 0.0f;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            return sampler;
        }

        //==============================================================================================================
        /// Returns the resource of `texture`, whose record the core accepted.
        ID3D12Resource* resourceOf(const core::TextureRecord& texture)
        {
            return static_cast<const d3d12::TexturePayload*>(texture.native)->resource;
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        void retireView([[maybe_unused]] void* context, void* native)
        {
            if (native == nullptr)
            {
                return;
            }
            static_cast<ID3D12Resource*>(native)->Release();
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<ViewIndex> Device::createView(Texture texture, const TextureViewDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::TextureRecord& record = core::checkView(impl.core, texture, desc);
        ID3D12Resource* resource = resourceOf(record);
        // The slot holds a reference of its own, released by the view hook when the slot retires.
        resource->AddRef();
        Result<ViewIndex> index = core::addView(impl.core, texture, resource);
        if (!index)
        {
            resource->Release();
            return index;
        }
        writeTextureView(impl, std::to_underlying(*index), resource, record, desc);
        return index;
    }

    //==================================================================================================================
    void Device::writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const core::TextureRecord& record = core::checkViewWrite(impl.core, range, index, texture, desc);
        ID3D12Resource* resource = resourceOf(record);
        resource->AddRef();
        if (!core::placeView(impl.core, range, index, texture, resource))
        {
            // A racing release took the range after the check; the slot is not written.
            resource->Release();
            return;
        }
        writeTextureView(impl, range.base + index, resource, record, desc);
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
        // A D3D12 sampler is the descriptor itself, so the slot holds no payload.
        Result<SamplerIndex> index = core::addSampler(impl.core, nullptr);
        if (!index)
        {
            return index;
        }
        const D3D12_SAMPLER_DESC sampler = samplerDesc(desc);
        impl.device()->CreateSampler(&sampler, impl.descriptors.samplerCpu(std::to_underlying(*index)));
        return index;
    }

    //==================================================================================================================
    void Device::writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkSamplerWrite(impl.core, range, index, desc);
        if (!core::placeSampler(impl.core, range, index, nullptr))
        {
            // A racing release took the range after the check; the slot is not written.
            return;
        }
        const D3D12_SAMPLER_DESC sampler = samplerDesc(desc);
        impl.device()->CreateSampler(&sampler, impl.descriptors.samplerCpu(range.base + index));
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
