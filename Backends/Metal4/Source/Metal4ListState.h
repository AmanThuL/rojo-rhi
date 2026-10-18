//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ListState.h
/// @brief Declares the native objects a Metal 4 command list owns and records into.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rojoRHI::metal4 {
    /// The native objects of one command list, created once when its frame slot's pool grows and kept while the pool hands the
    /// list out again: its own allocator, the command buffer it records into, its argument table and its kernel table.
    /// Everything recording touches is the list's own, so recording takes no lock. Each pointer is one retained reference.
    struct ListState
    {
        MTL4::CommandAllocator* allocator = nullptr;  ///< The list's allocator, reset when its frame slot is reused.
        MTL4::CommandBuffer* commandBuffer = nullptr; ///< Begun at each acquisition, ended by `CommandList::end`, committed by `Queue::submit`.
        /// Four buffer bindings: `buffer(1)` and `buffer(2)` hold the view and sampler tables' GPU addresses from creation.
        MTL4::ArgumentTable* table = nullptr;
        /// Two buffer bindings and one texture binding, which a backend kernel binds before each dispatch.
        MTL4::ArgumentTable* kernelTable = nullptr;
        NS::String* label = nullptr;          ///< The list's label as the command buffer carries it; replaced at each acquisition.
        std::vector<NS::String*> scopeLabels; ///< The label of each open scope's debug group, innermost last.
        std::string scratch;                  ///< A NUL-terminated copy of a scope label, made before its string is.

        /// Creates the allocator, the command buffer, the argument table and the kernel table on `device`, binding the view
        /// table at `viewTableAddress` to `buffer(1)` and the sampler table at `samplerTableAddress` to `buffer(2)` (ADR 0018).
        /// On failure it releases what it created and names the object Metal did not make.
        Result<void> create(MTL::Device* device, uint64_t viewTableAddress, uint64_t samplerTableAddress);

        /// Releases every object that exists, in reverse order of creation, and clears each pointer. The caller holds an
        /// autorelease pool and no queue holds work that uses them.
        void release();
    };
} // namespace rojoRHI::metal4
