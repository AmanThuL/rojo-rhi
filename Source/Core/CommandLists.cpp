//----------------------------------------------------------------------------------------------------------------------
/// @file CommandLists.cpp
/// @brief Implements the recording state machine of a command list and the list rules of acquiring, ending and submitting.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/CommandLists.h"

#include "Base/Assert.h"
#include "Core/Accesses.h"
#include "Core/Label.h"
#include "Core/Tracker/References.h"

#include <bit>
#include <utility>

namespace rojoRHI::core {
    //==================================================================================================================
    void checkAcquire([[maybe_unused]] const DeviceCore& core)
    {
        ROJORHI_VU(core.frameOpen(), "VU-rojo-cmd-0001", "a command list was acquired with no frame open");
    }

    //==================================================================================================================
    void beginList(CommandListCore& list, DeviceCore& core, QueueType type, std::string_view label)
    {
        // Every field starts afresh, but the containers are cleared and moved into the new state, so a list the pool hands
        // out again keeps their capacity.
        // A list that was never submitted still holds the counts it added; its slot is reused now, so they go before the log is cleared.
        releaseReferences(list);
        list.pass.colors.clear();
        list.scopes.clear();
        list.scopeLog.clear();
        list.pendingAccesses.clear();
        list.trackerLog.events.clear();
        list.trackerLog.commands.clear();
        list.trackerLog.command = 0;
        list.trackerLog.passUses.clear();
        list.trackerLog.images.clear();
        list = CommandListCore{ .pass = PassState{ .colors = std::move(list.pass.colors) },
                                .scopes = std::move(list.scopes),
                                .scopeLog = std::move(list.scopeLog),
                                .pendingAccesses = std::move(list.pendingAccesses),
                                .trackerLog = std::move(list.trackerLog) };
        list.label = resolveLabel(label, "rojorhi.commandlist.unnamed");
        list.device = &core;
        list.type = type;
        list.frame = core.frameNumber();
        core.noteListAcquired();
    }

    //==================================================================================================================
    void checkRecording([[maybe_unused]] const CommandListCore& list)
    {
        ROJORHI_VU(list.state == ListState::Recording, "VU-rojo-cmd-0063", "a command was recorded on the list \"{}\", which is not recording", list.label);
    }

    //==================================================================================================================
    void endList(CommandListCore& list)
    {
        ROJORHI_VU(list.state == ListState::Recording, "VU-rojo-cmd-0002", "end was called on the list \"{}\", which is not recording", list.label);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0003", "end was called on the list \"{}\" with a render pass open", list.label);
        ROJORHI_VU(list.scopes.empty(), "VU-rojo-cmd-0004", "end was called on the list \"{}\" with {} scopes still open", list.label, list.scopes.size());
        checkNoPendingAccesses(list);
        list.state = ListState::Ended;
    }

    //==================================================================================================================
    void checkSameDevice([[maybe_unused]] const DeviceCore& owner, [[maybe_unused]] const DeviceCore& other, [[maybe_unused]] std::string_view kind)
    {
        ROJORHI_VU(&owner == &other, "VU-rojo-core-0005", "a {} of device \"{}\" was used with device \"{}\"", kind, other.label(), owner.label());
    }

    //==================================================================================================================
    void submitLists(DeviceCore& core, [[maybe_unused]] QueueType type, std::span<const CommandList> lists)
    {
        for (const CommandList& handle : lists)
        {
            CommandListCore& list = coreOf(handle);
            checkSameDevice(core, *list.device, "CommandList");
            ROJORHI_VU(
                list.type == type,
                "VU-rojo-queue-0002",
                "the list \"{}\" was acquired for queue type {} and was submitted to queue type {}",
                list.label,
                static_cast<int>(list.type),
                static_cast<int>(type)
            );
            ROJORHI_VU(list.state != ListState::Recording, "VU-rojo-queue-0001", "the list \"{}\" was submitted before it ended", list.label);
            // Device::endFrame requires every list of the frame to be submitted, so a list of a closed frame that reaches this
            // check is one already submitted; the frame clause comes first so that case reports the frame, which is what the
            // caller got wrong, and the "submitted before" clause below covers a resubmission within the open frame.
            ROJORHI_VU(
                core.frameOpen() && list.frame == core.frameNumber(),
                "VU-rojo-queue-0001",
                "the list \"{}\" was acquired in frame {} and was submitted in frame {}, which is not the open frame",
                list.label,
                list.frame,
                core.frameNumber()
            );
            ROJORHI_VU(list.state != ListState::Submitted, "VU-rojo-queue-0001", "the list \"{}\" was submitted before", list.label);
            list.state = ListState::Submitted;
            core.submitScopes(list);
            core.counts().fold(list.counts);
            core.noteListSubmitted();
        }
    }
    //==================================================================================================================
    ArenaAllocation allocateFrameData(
        DeviceCore& core, CommandListCore& list, std::span<const std::byte> bytes, uint32_t alignment, ArenaPageSource source, void* context
    )
    {
        ROJORHI_VU(!bytes.empty(), "VU-rojo-buffer-0015", "uploadFrameData was called with no bytes");
        ROJORHI_VU(std::has_single_bit(alignment), "VU-rojo-buffer-0016", "the alignment {} is not a power of two", alignment);
        ROJORHI_VU(core.frameOpen(), "VU-rojo-buffer-0017", "uploadFrameData was called with no frame open");
        checkRecording(list);
        ++list.counts.frameDataUploads;
        list.counts.frameArenaBytes += bytes.size();
        return bumpArenaPage(core, list, bytes.size(), alignment, source, context);
    }

    //==================================================================================================================
    void claimArenaPage(DeviceCore& core, CommandListCore& list, uint64_t size, ArenaPageSource source, void* context)
    {
        const uint32_t slot = static_cast<uint32_t>(list.frame % core.framesInFlight());
        list.page = core.arena().claim(slot, size, core.capabilities().limits.maxBufferSize, source, context);
        core.pool<Buffer>().update(list.page.buffer, [&](BufferRecord& record) { record.arenaFrame = list.frame; });
        ROJORHI_INTERNAL_ASSERT(
            reinterpret_cast<uintptr_t>(list.page.host) % kArenaPageAlignment == 0, "an arena page's host memory is not aligned to kArenaPageAlignment"
        );
        list.cursor = list.page.host;
        list.limit = list.page.host + list.page.size;
    }

    //==================================================================================================================
    ArenaAllocation bumpArenaPageSlow(DeviceCore& core, CommandListCore& list, uint64_t size, uint32_t alignment, ArenaPageSource source, void* context)
    {
        uint64_t offset = list.page.host == nullptr ? 0 : (pageUsedBytes(list) + alignment - 1) & ~(uint64_t{ alignment } - 1);
        if (list.page.host == nullptr || offset + size > list.page.size)
        {
            // A new page's offset zero satisfies every alignment.
            claimArenaPage(core, list, size, source, context);
            offset = 0;
        }
        list.cursor = list.page.host + offset + size;
        return ArenaAllocation{ list.page.buffer, offset, list.page.host + offset, list.page.address + offset };
    }
} // namespace rojoRHI::core
