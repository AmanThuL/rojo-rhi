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
    /// How a list names an object it references; each kind is counted apart (VU-rojo-abi-0003, VU-rojo-buffer-0019,
    /// VU-rojo-device-0007).
    enum class RefKind : uint8_t
    {
        Root,  ///< `setRoot` was given an address in the buffer.
        Addr,  ///< A recorded command was passed an address in the buffer.
        Handle ///< A recorded command names the object by its handle.
    };

    /// Does the work of `noteReference` in a Debug build.
    void noteReferenceChecked(CommandListCore& list, RefKind kind, uint64_t handleBits);

    /// Does the work of `releaseReferences` in a Debug build.
    void releaseReferencesChecked(CommandListCore& list);

    /// Does the work of `releaseSubmitted` in a Debug build.
    void releaseSubmittedChecked(std::span<const CommandList> lists);

    /// Does the work of `checkUnreferenced` in a Debug build.
    template<typename Handle> void checkUnreferencedChecked(DeviceCore& core, Handle handle);

    /// Counts the object `handle` among the unsubmitted references of `list`, as `kind` names it, once per list: a second call
    /// with the same object and kind adds nothing. A null handle, and an address into a frame-arena page, count nothing. Takes
    /// the object's pool lock. Inline, so a Release build compiles the call to nothing; a device without validation counts nothing.
    template<typename Handle> inline void noteReference([[maybe_unused]] CommandListCore& list, [[maybe_unused]] RefKind kind, [[maybe_unused]] Handle handle)
    {
#ifndef NDEBUG
        noteReferenceChecked(list, kind, handle.bits);
#endif
    }

    /// Removes the counts `list` added, so the objects it referenced may be destroyed; `releaseSubmitted` calls it, and so does
    /// the recycling of a list that was never submitted. A list holds no counts afterwards. Inline, as `noteReference` is.
    inline void releaseReferences([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        releaseReferencesChecked(list);
#endif
    }

    /// Removes the counts of every list of `lists`, which `submitLists` accepted and the tracker replayed. A backend calls it last
    /// in `Queue::submit`, after the tracker's lock is released: each count lives under its object's pool lock, and a submission
    /// takes no pool lock but the fences' under the tracker's. Inline, as `noteReference` is.
    inline void releaseSubmitted([[maybe_unused]] std::span<const CommandList> lists)
    {
#ifndef NDEBUG
        releaseSubmittedChecked(lists);
#endif
    }
} // namespace rojoRHI::core
