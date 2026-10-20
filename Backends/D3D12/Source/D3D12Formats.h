//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Formats.h
/// @brief Declares the D3D12 translation of a Format: resource, view and depth-stencil formats, and the queried format table.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "D3D12Common.h"

#include <dxgiformat.h>

#include <vector>

namespace rojoRHI::d3d12 {
    /// Returns the `DXGI_FORMAT` that is `format` itself, one to one; `Format::Undefined` is `DXGI_FORMAT_UNKNOWN`.
    DXGI_FORMAT dxgiFormat(Format format);

    /// Returns the format a texture of `format` and `usage` is created with. A depth texture that is `Sampled` is created in
    /// the typeless family of its depth format, so its depth and stencil planes get shader views. A format with an sRGB
    /// counterpart is created in the typeless family of both when `relaxedCasting` is false, so a view can use either; with
    /// relaxed casting it is created as itself and `castableFormats` lists what its views may use. Any other texture is
    /// created as `dxgiFormat(format)`.
    DXGI_FORMAT resourceFormat(Format format, TextureUsage usage, bool relaxedCasting);

    /// Returns the formats a view of a texture created as `format` may use when the device casts formats: `format` and its sRGB
    /// counterpart, or nothing when the format has none.
    std::vector<DXGI_FORMAT> castableFormats(Format format);

    /// Returns the format of a shader view (SRV or UAV) of a texture of `texture` format. `view` is the format the view asks
    /// for, `Format::Undefined` for the texture's own. The depth aspect of a depth texture is read as a float, its stencil
    /// aspect as the stencil plane of a combined format. A `Storage` view of an sRGB format uses its linear counterpart,
    /// since a typed UAV has no sRGB form.
    DXGI_FORMAT viewFormat(Format texture, Format view, TextureAspect aspect, TextureUsage viewUsage);

    /// Returns the format of the depth-stencil view of a depth texture of `format`.
    DXGI_FORMAT dsvFormat(Format format);

    /// Returns what `device` supports for each format: the core's ceiling for the format, narrowed by what the device
    /// reports. No format reports a sparse capability.
    core::FormatTable queryFormatTable(ID3D12Device12* device);
} // namespace rojoRHI::d3d12
