//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Memory.h
/// @brief Declares the translation of RojoRHI memory classes into Metal resource options and storage modes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    MTL::ResourceOptions resourceOptions(Memory memory);

    MTL::ResourceOptions placedOptions(Memory memory);

    MTL::StorageMode storageMode(Memory memory);
} // namespace rojoRHI::metal4
