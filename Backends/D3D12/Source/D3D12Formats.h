//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Formats.h
/// @brief Declares the D3D12 translation of a Format: resource, view and depth-stencil formats, and the queried format table.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "D3D12Common.h"

#include <dxgiformat.h>

#include <span>

namespace rojoRHI::d3d12 {
    DXGI_FORMAT dxgiFormat(Format format);

    // A Sampled depth texture is created in the typeless family of its depth format, so its depth and stencil planes
    // get shader views. Without relaxed casting, a format with an sRGB counterpart is created in the typeless family of
    // both, so a view can use either; with it, as itself, and `castableFormats` lists what its views may use.
    DXGI_FORMAT resourceFormat(Format format, TextureUsage usage, bool relaxedCasting);

    // `format` and its sRGB counterpart, or nothing when it has none. The span views static storage.
    std::span<const DXGI_FORMAT> castableFormats(Format format);

    // `view` is `Format::Undefined` for the texture's own. A depth aspect reads as a float, a stencil aspect as the
    // stencil plane of a combined format. A `Storage` view of an sRGB format uses its linear counterpart: a typed UAV
    // has no sRGB form.
    DXGI_FORMAT viewFormat(Format texture, Format view, TextureAspect aspect, TextureUsage viewUsage);

    DXGI_FORMAT dsvFormat(Format format);

    // The core's ceiling narrowed by what the device reports; a sparse format has no stencil plane and reports
    // D3D12_FORMAT_SUPPORT2_TILED. A query the runtime fails is an error, never a narrower table.
    Result<core::FormatTable> queryFormatTable(ID3D12Device12* device);
} // namespace rojoRHI::d3d12
