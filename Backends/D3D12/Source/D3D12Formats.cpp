//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Formats.cpp
/// @brief Implements the D3D12 translation of a Format and the format table a device reports.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Formats.h"

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "D3D12Common.h"

#include <cstdint>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        //==============================================================================================================
        /// Returns true when `format` is an sRGB-encoded member of a pair `srgbCounterpart` knows.
        bool isSrgb(Format format)
        {
            return format == Format::RGBA8Unorm_sRGB || format == Format::BC1Unorm_sRGB;
        }

        //==============================================================================================================
        /// Returns true when `flags` has every bit of `flag`.
        bool has(UINT flags, UINT flag)
        {
            return (flags & flag) == flag;
        }

        //==============================================================================================================
        /// Returns the format support `device` reports for `format`; a format the runtime refuses to describe supports nothing.
        D3D12_FEATURE_DATA_FORMAT_SUPPORT supportOf(ID3D12Device12* device, DXGI_FORMAT format)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
            support.Format = format;
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, static_cast<UINT>(sizeof(support)))))
            {
                support.Support1 = D3D12_FORMAT_SUPPORT1_NONE;
                support.Support2 = D3D12_FORMAT_SUPPORT2_NONE;
            }
            return support;
        }

        //==============================================================================================================
        /// Returns true when `device` offers at least one quality level of `format` at `samples` samples.
        bool multisampled(ID3D12Device12* device, DXGI_FORMAT format, uint32_t samples)
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
            levels.Format = format;
            levels.SampleCount = samples;
            levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
            return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, static_cast<UINT>(sizeof(levels)))) &&
                   levels.NumQualityLevels > 0;
        }

        //==============================================================================================================
        /// Returns what `device` supports for `format`, never more than the core's ceiling.
        FormatCapabilities queryFormat(ID3D12Device12* device, Format format)
        {
            const FormatCapabilities ceiling = core::formatCeiling(format);
            FormatCapabilities caps;
            if (format == Format::Undefined)
            {
                return caps;
            }
            // A depth texture is sampled through its float view, so that view's support is what sampling needs.
            const bool depth = core::isDepthFormat(format);
            const DXGI_FORMAT native = dxgiFormat(format);
            const D3D12_FEATURE_DATA_FORMAT_SUPPORT shader =
                supportOf(device, depth ? viewFormat(format, Format::Undefined, TextureAspect::Depth, TextureUsage::Sampled) : native);
            const D3D12_FEATURE_DATA_FORMAT_SUPPORT target = depth ? supportOf(device, native) : shader;
            const UINT shader1 = static_cast<UINT>(shader.Support1);
            const UINT shader2 = static_cast<UINT>(shader.Support2);
            const UINT target1 = static_cast<UINT>(target.Support1);
            // SHADER_SAMPLE is what a filtered read needs, SHADER_LOAD what an unfiltered one needs (Support1).
            caps.filterLinear = ceiling.filterLinear && has(shader1, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
            caps.sampled = ceiling.sampled && (caps.filterLinear || has(shader1, D3D12_FORMAT_SUPPORT1_SHADER_LOAD));
            // Typed UAV loads and stores are separate bits of Support2.
            caps.storageRead = ceiling.storageRead && has(shader2, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD);
            caps.storageWrite = ceiling.storageWrite && has(shader2, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
            // RENDER_TARGET and DEPTH_STENCIL (Support1) gate the attachment roles; blending and resolving apply to a color
            // attachment only.
            caps.colorAttachment = ceiling.colorAttachment && has(target1, D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
            caps.depthAttachment = ceiling.depthAttachment && has(target1, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL);
            caps.blend = caps.colorAttachment && ceiling.blend && has(target1, D3D12_FORMAT_SUPPORT1_BLENDABLE);
            caps.resolve = caps.colorAttachment && ceiling.resolve && has(target1, D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
            // Sparse stays clear until the backend places tiles.
            caps.sparse = false;
            // One sample always works; D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS answers 2, 4 and 8 for an attachment.
            caps.sampleCounts = 1;
            if (caps.colorAttachment || caps.depthAttachment)
            {
                for (uint32_t bit = 1; bit < 4; ++bit)
                {
                    if (((ceiling.sampleCounts >> bit) & 1u) != 0 && multisampled(device, native, 1u << bit))
                    {
                        caps.sampleCounts |= 1u << bit;
                    }
                }
            }
            return caps;
        }
    } // namespace

    //==================================================================================================================
    DXGI_FORMAT dxgiFormat(Format format)
    {
        switch (format)
        {
        case Format::Undefined: return DXGI_FORMAT_UNKNOWN;
        case Format::BGRA8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8Unorm_sRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RG16Float: return DXGI_FORMAT_R16G16_FLOAT;
        case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
        case Format::BC1Unorm: return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1Unorm_sRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
        case Format::R16Float: return DXGI_FORMAT_R16_FLOAT;
        case Format::R32Float: return DXGI_FORMAT_R32_FLOAT;
        case Format::D32FloatS8: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case Format::RGB10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    //==================================================================================================================
    DXGI_FORMAT resourceFormat(Format format, TextureUsage usage, bool relaxedCasting)
    {
        const bool sampled = (usage & TextureUsage::Sampled) == TextureUsage::Sampled;
        if (core::isDepthFormat(format) && sampled)
        {
            return format == Format::D32Float ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
        }
        if (!relaxedCasting && core::srgbCounterpart(format) != Format::Undefined)
        {
            return core::isBlockCompressed(format) ? DXGI_FORMAT_BC1_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        }
        return dxgiFormat(format);
    }

    //==================================================================================================================
    std::vector<DXGI_FORMAT> castableFormats(Format format)
    {
        const Format counterpart = core::srgbCounterpart(format);
        if (counterpart == Format::Undefined)
        {
            return {};
        }
        return { dxgiFormat(format), dxgiFormat(counterpart) };
    }

    //==================================================================================================================
    DXGI_FORMAT viewFormat(Format texture, Format view, TextureAspect aspect, TextureUsage viewUsage)
    {
        if (aspect == TextureAspect::Stencil)
        {
            return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        }
        if (aspect == TextureAspect::Depth)
        {
            return texture == Format::D32Float ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        }
        Format chosen = view == Format::Undefined ? texture : view;
        if (viewUsage == TextureUsage::Storage && isSrgb(chosen))
        {
            chosen = core::srgbCounterpart(chosen);
        }
        return dxgiFormat(chosen);
    }

    //==================================================================================================================
    DXGI_FORMAT dsvFormat(Format format)
    {
        return format == Format::D32Float ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }

    //==================================================================================================================
    core::FormatTable queryFormatTable(ID3D12Device12* device)
    {
        core::FormatTable table;
        for (uint32_t value = 0; value < core::kFormatCount; ++value)
        {
            table[value] = queryFormat(device, static_cast<Format>(value));
        }
        return table;
    }
} // namespace rojoRHI::d3d12
