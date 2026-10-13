//----------------------------------------------------------------------------------------------------------------------
/// @file CommandLists.h
/// @brief Declares the recording state of a command list and the checks of acquiring, ending and submitting one.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CommandList.h>
#include <rojoRHI/QueueType.h>

#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::core {
    /// Where a command list is in its life: recording from acquisition, ended by `CommandList::end`, then submitted.
    enum class ListState : uint8_t
    {
        Recording, ///< Acquired and not yet ended.
        Ended,     ///< Ended and not yet submitted.
        Submitted  ///< Handed to a queue; it records nothing further.
    };

    /// What the core keeps about one command list: the state machine the recording rules read. A backend's `CommandListImpl`
    /// embeds one. Later parts add the bound pipeline and root.
    struct CommandListCore
    {
        DeviceCore* device = nullptr;         ///< The device that acquired the list.
        QueueType type = QueueType::Graphics; ///< The queue type the list was acquired for.
        uint64_t frame = 0;                   ///< The frame the list was acquired in.
        ListState state = ListState::Recording;
        bool passOpen = false;   ///< A render pass is open on the list.
        uint32_t openScopes = 0; ///< Scopes begun on the list and not yet ended.
        std::string label;       ///< The label the list was acquired with.
        ArenaPage page;          ///< The arena page the list uploads into; null until its first upload.
        uint64_t pageUsed = 0;   ///< Bytes of `page` the list has used: its bump cursor, which takes no lock.
    };

    /// Returns the core of the command list `list` wraps (VU-rojo-core-0004). Each backend defines it, as it does for a
    /// device.
    CommandListCore& coreOf(CommandList list);

    /// Checks that a frame is open (VU-rojo-cmd-0001); a backend calls it first in `Device::acquireCommandList`.
    void checkAcquire(const DeviceCore& core);

    /// Starts `list`, which a backend took from its pool, as a recording list of `type` in the open frame: it checks the label
    /// (VU-rojo-core-0006, VU-rojo-core-0007), resets every field and counts the list as unsubmitted for
    /// VU-rojo-device-0005.
    void beginList(CommandListCore& list, DeviceCore& core, QueueType type, std::string_view label);

    /// Checks that `list` can end (VU-rojo-cmd-0002 to 0004) and marks it ended.
    void endList(CommandListCore& list);

    /// Checks every list of a submission to a queue of `type` on the device `core` (VU-rojo-core-0004, VU-rojo-core-0005,
    /// VU-rojo-queue-0001, VU-rojo-queue-0002) and marks each one submitted. A backend calls it first in `Queue::submit`.
    void submitLists(DeviceCore& core, QueueType type, std::span<const CommandList> lists);

    /// Where an upload landed: the page's buffer, the byte offset inside it and the memory to copy the bytes to.
    struct ArenaAllocation
    {
        Buffer page;       ///< The page's buffer.
        uint64_t offset;   ///< Bytes from the start of the page, a multiple of the requested alignment.
        std::byte* memory; ///< The host address of that offset.
    };

    /// Checks an upload of `bytes` at `alignment` (VU-rojo-buffer-0015 to 0017) and bumps the cursor of `list`'s own page; only
    /// a list with no page, or whose page is full, claims a page from the arena, which takes a lock and may grow it through
    /// `source`. The backend copies `bytes` to `memory` and encodes the address.
    ArenaAllocation allocateFrameData(
        DeviceCore& core, CommandListCore& list, std::span<const std::byte> bytes, uint32_t alignment, ArenaPageSource source, void* context
    );

    /// Issues VU-rojo-core-0005 when `other`, the device an object of kind `kind` belongs to, is not `owner`.
    void checkSameDevice(const DeviceCore& owner, const DeviceCore& other, std::string_view kind);
} // namespace rojoRHI::core
