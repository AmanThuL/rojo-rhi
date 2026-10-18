//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Formats.h
/// @brief Declares the Metal 4 pixel format of each `Format`, its inverse, and the format table a Metal 4 device reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    /// Returns the `MTL::PixelFormat` chapter 15 maps `format` to; `Format::Undefined` is `MTL::PixelFormatInvalid`.
    MTL::PixelFormat pixelFormat(Format format);

    /// Returns the `Format` chapter 15 maps to `pixelFormat`, the inverse of `pixelFormat`; `Format::Undefined` for a pixel
    /// format RojoRHI does not map.
    Format formatOf(MTL::PixelFormat pixelFormat);

    /// Returns the format table of `device`: `core::formatCeilings()` narrowed by what the device and its families support, with
    /// every sparse capability clear.
    core::FormatTable metalFormatTable(MTL::Device* device);
} // namespace rojoRHI::metal4
