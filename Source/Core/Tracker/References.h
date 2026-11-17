//----------------------------------------------------------------------------------------------------------------------
/// @file References.h
/// @brief Declares the hooks that count the objects an unsubmitted command list references, and the check that reads the counts.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

#include <cstdint>
#include <span>

namespace rojoRHI::core {
    // Each kind is counted apart (VU-rojo-abi-0003, VU-rojo-buffer-0019, VU-rojo-device-0007).
    enum class RefKind : uint8_t
    {
        Root,  // `setRoot` was given an address in the buffer.
        Addr,  // A recorded command was passed an address in the buffer.
        Handle // A recorded command names the object by its handle.
    };

    void noteReferenceChecked(CommandListCore& list, RefKind kind, uint64_t handleBits);

    void releaseReferencesChecked(CommandListCore& list);

    void releaseSubmittedChecked(std::span<const CommandList> lists);

    template<typename Handle> void checkUnreferencedChecked(DeviceCore& core, Handle handle);

    // Counts once per list, object and kind; a null handle or a frame-arena address counts nothing. Takes the object's pool
    // lock. Inline, so a Release build compiles the call to nothing; a device without validation counts nothing.
    template<typename Handle> inline void noteReference([[maybe_unused]] CommandListCore& list, [[maybe_unused]] RefKind kind, [[maybe_unused]] Handle handle)
    {
#ifndef NDEBUG
        noteReferenceChecked(list, kind, handle.bits);
#endif
    }

    // Called by `releaseSubmitted`, and when a list that was never submitted is recycled.
    inline void releaseReferences([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        releaseReferencesChecked(list);
#endif
    }

    // A backend calls it last in `Queue::submit`, after the tracker's lock is released: each count lives under its object's
    // pool lock, and a submission takes no pool lock but the fences' under the tracker's.
    inline void releaseSubmitted([[maybe_unused]] std::span<const CommandList> lists)
    {
#ifndef NDEBUG
        releaseSubmittedChecked(lists);
#endif
    }
} // namespace rojoRHI::core
