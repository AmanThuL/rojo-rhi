//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Formats.h
/// @brief Declares the Metal 4 pixel format of each `Format`, its inverse, and the format table a Metal 4 device reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    MTL::PixelFormat pixelFormat(Format format);

    Format formatOf(MTL::PixelFormat pixelFormat);

    core::FormatTable metalFormatTable(MTL::Device* device);
} // namespace rojoRHI::metal4
