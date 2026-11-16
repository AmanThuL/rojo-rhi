//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Memory.h
/// @brief Declares the translation of RojoRHI memory classes into Metal resource options and storage modes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    /// Returns the options of a dedicated allocation in `memory`: `Gpu` is Private; `Upload` is Shared with the default CPU
    /// cache mode, never write-combined; `Readback` is Shared.
    MTL::ResourceOptions resourceOptions(Memory memory);

    /// Returns the options of a resource placed in a heap of `memory`: those of `resourceOptions` with untracked hazards,
    /// matching the heap descriptor.
    MTL::ResourceOptions placedOptions(Memory memory);

    /// Returns the storage mode of `memory`: Private for `Gpu`, Shared otherwise.
    MTL::StorageMode storageMode(Memory memory);
} // namespace rojoRHI::metal4
