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
#include <string_view>
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
        /// The list's label as the command buffer carries it; kept while the next acquisition's label has the same text.
        NS::String* label = nullptr;
        std::string labelText; ///< The text `label` was made from.
        /// True when the command buffer carries `label`: it keeps its label across recordings, so it is set only when it changes.
        bool labelSet = false;
        std::vector<NS::String*> scopeLabels; ///< The label of each open scope's debug group, innermost last.
        /// The timestamp pair each open scope claimed, innermost last: the index of its begin entry, or `core::kNoTimestamp`.
        std::vector<uint32_t> scopeTimestamps;
        /// True when each native command the list records opens a `CommandPool`: the device's `commandPools`, copied at creation
        /// so a recording entry reads it from the list's own state.
        bool commandPools = false;
        bool encoderLabels = false; ///< True when the encoders the list opens carry labels: the device's `encoderLabels`.
        std::string scratch;        ///< A NUL-terminated copy of a scope label, made before its string is.

        /// The debug-group string of one scope or pass of a recording, by the order in which they began, with the label text
        /// it was made from, so the next recording reuses it while the text is the same. Each holds one reference.
        struct GroupLabel
        {
            std::string text;             ///< The scope's label.
            NS::String* string = nullptr; ///< Its string, or the fallback's when the label made none.
        };

        std::vector<GroupLabel> groupLabels; ///< The strings of the scopes and passes of the recordings so far, in order.
        size_t groupCursor = 0;              ///< How many scopes and passes the current recording has begun.

        /// Creates the allocator, the command buffer, the argument table and the kernel table on `device`, binding the view
        /// table at `viewTableAddress` to `buffer(1)` and the sampler table at `samplerTableAddress` to `buffer(2)` (ADR 0018). `perCommandPools` is stored as
        /// the list's `commandPools` and `labelEncoders` as its `encoderLabels`. On failure it releases what it created and names
        /// the object Metal did not make.
        Result<void> create(MTL::Device* device, uint64_t viewTableAddress, uint64_t samplerTableAddress, bool perCommandPools, bool labelEncoders);

        /// Releases every object that exists, in reverse order of creation, and clears each pointer. The caller holds an
        /// autorelease pool and no queue holds work that uses them.
        void release();

        /// Returns a string holding `label`, for the debug group of the recording's next scope or pass and the labels of the
        /// encoders it encloses; the caller owns one reference. The string the previous recordings made for the scope or pass
        /// that began in the same order is reused when its text is `label`; otherwise a new one replaces it, made with alloc
        /// and init, so it is not autoreleased and needs no pool. The label is not NUL-terminated, so it is copied into
        /// `scratch` first. A label that is not valid UTF-8 makes no string, and a debug group needs one, so the string then
        /// holds `fallback`.
        NS::String* makeGroupLabel(std::string_view label, const char* fallback);
    };
} // namespace rojoRHI::metal4
