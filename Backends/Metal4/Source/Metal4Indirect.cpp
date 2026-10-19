//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Indirect.cpp
/// @brief Implements the indirect draw entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Metal4Common.h"

#include <cstdint>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::drawIndirect([[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawIndirect");
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(
        [[maybe_unused]] Buffer indices,
        [[maybe_unused]] IndexType type,
        [[maybe_unused]] Addr args,
        [[maybe_unused]] uint32_t maxCount,
        [[maybe_unused]] Addr count
    )
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawIndexedIndirect");
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect([[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawMeshTasksIndirect");
    }
} // namespace rojoRHI
