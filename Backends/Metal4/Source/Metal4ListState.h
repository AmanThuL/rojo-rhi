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
    // Created once when the frame slot's pool grows and kept while the pool hands the list out again. Everything
    // recording touches is the list's own, so recording takes no lock. Each pointer is one retained reference.
    struct ListState
    {
        MTL4::CommandAllocator* allocator = nullptr;  // Reset when its frame slot is reused.
        MTL4::CommandBuffer* commandBuffer = nullptr; // Begun at each acquisition, ended by `CommandList::end`, committed by `Queue::submit`.
        // `buffer(1)` and `buffer(2)` hold the view and sampler tables' GPU addresses from creation.
        MTL4::ArgumentTable* table = nullptr;
        // A backend kernel binds it before each dispatch.
        MTL4::ArgumentTable* kernelTable = nullptr;
        // Kept while the next acquisition's label has the same text.
        NS::String* label = nullptr;
        std::string labelText;
        // The command buffer keeps its label across recordings, so it is set only when it changes.
        bool labelSet = false;
        std::vector<NS::String*> scopeLabels; // Innermost last.
        // Innermost last: the index of each begin entry, or `core::kNoTimestamp`.
        std::vector<uint32_t> scopeTimestamps;
        // Copied from the device at creation, so a recording entry reads the list's own state.
        bool commandPools = false;
        bool markers = false; // The device's `markers`.
        std::string scratch;  // NUL-terminated, made before its string is.

        // One per scope or pass of a recording, by the order in which they began, so the next recording reuses a string
        // while its text is the same. Each holds one reference.
        struct GroupLabel
        {
            std::string text;
            NS::String* string = nullptr; // Or the fallback's when the label made none.
        };

        std::vector<GroupLabel> groupLabels;
        size_t groupCursor = 0; // The scopes and passes the current recording has begun.

        // On failure it releases what it created and names the object Metal did not make.
        Result<void> create(
            MTL::Device* device, uint64_t viewTableAddress, uint64_t samplerTableAddress, uint64_t zeroRecordAddress, bool perCommandPools, bool emitMarkers
        );

        // The caller holds an autorelease pool and no queue holds work that uses them.
        void release();

        // The caller owns one reference. A string made with alloc and init is not autoreleased and needs no pool. A
        // label that is not valid UTF-8 makes no string, and a debug group needs one, so the string then holds
        // `fallback`.
        NS::String* makeGroupLabel(std::string_view label, const char* fallback);
    };
} // namespace rojoRHI::metal4
