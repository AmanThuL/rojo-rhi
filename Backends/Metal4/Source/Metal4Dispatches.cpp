//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Dispatches.cpp
/// @brief Implements the dispatch entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Draws.h"
#include "Metal4Common.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4PipelineState.h"

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::dispatch(list.core, x, y, z);
        if (x == 0 || y == 0 || z == 0)
        {
            return;
        }
        MTL4::ComputeCommandEncoder* encoder = metal4::applyComputePipeline(list);
        {
            // The Metal validation layer autoreleases in a dispatch it checks, so the dispatch holds a pool.
            const metal4::AutoreleasePool pool;
            encoder->dispatchThreadgroups(MTL::Size(x, y, z), list.draw.compute->computeThreads);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect(Addr args)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::dispatchIndirect(list.core, args);
        MTL4::ComputeCommandEncoder* encoder = metal4::applyComputePipeline(list);
        {
            // The validation layer autoreleases in a dispatch it checks.
            const metal4::AutoreleasePool pool;
            encoder->dispatchThreadgroups(uint64_t{ args.lo } | uint64_t{ args.hi } << 32, list.draw.compute->computeThreads);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
    }
} // namespace rojoRHI
