//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Memory.cpp
/// @brief Implements the translation of RojoRHI memory classes into Metal resource options and storage modes.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Memory.h"

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    //==================================================================================================================
    MTL::ResourceOptions resourceOptions(Memory memory)
    {
        switch (memory)
        {
        case Memory::Gpu: return MTL::ResourceStorageModePrivate;
        case Memory::Upload:
            // Scattered small CPU writes are slow through write-combined memory, so Upload keeps the default cache mode.
            return MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache;
        case Memory::Readback: return MTL::ResourceStorageModeShared;
        }
        return MTL::ResourceStorageModePrivate;
    }

    //==================================================================================================================
    MTL::ResourceOptions placedOptions(Memory memory)
    {
        return resourceOptions(memory) | MTL::ResourceHazardTrackingModeUntracked;
    }

    //==================================================================================================================
    MTL::StorageMode storageMode(Memory memory)
    {
        return memory == Memory::Gpu ? MTL::StorageModePrivate : MTL::StorageModeShared;
    }
} // namespace rojoRHI::metal4
