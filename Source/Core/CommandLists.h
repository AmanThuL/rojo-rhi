//----------------------------------------------------------------------------------------------------------------------
/// @file CommandLists.h
/// @brief Declares the recording state of a command list and the checks of acquiring, ending and submitting one.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"
#include "Core/ScopeLog.h"
#include "Core/Tracker/TrackerLog.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::core {
    enum class ListState : uint8_t
    {
        Recording,
        Ended,
        Submitted
    };

    struct PassState
    {
        std::vector<Format> colors;
        Format depthFormat = Format::Undefined; // `Undefined` for none.
        uint32_t samples = 1;                   // Of every attachment except a resolve target.
        Rect2D area;
        bool viewportSet = false; // Since the pass began.
        bool scissorSet = false;  // Since the pass began.
    };

    struct BoundPipeline
    {
        bool bound = false;
        PipelineKind kind = PipelineKind::Graphics;
        uint32_t stages = 0;                                 // Bit `1 << ShaderStage` per stage the pipeline has.
        PrimitiveClass primitive = PrimitiveClass::Triangle; // Of a vertex pipeline.
        bool depthBias = false;
        uint32_t blendMask = 0; // Bit `i` is set when color target `i` blends.
        bool depthWrite = false;
        bool stencilWrite = false; // Some stencil operation is not `Keep`.
    };

    // The fields that hold no container and no string: `beginList` resets them all with one assignment of
    // `ListFields{}`. A field that owns memory belongs in `CommandListCore`, which clears each of its own by name.
    struct ListFields
    {
        DeviceCore* device = nullptr;
        QueueType type = QueueType::Graphics;
        uint64_t frame = 0;
        Addr root = {};         // Null until `setRoot`.
        Buffer rootBuffer = {}; // Written in Debug on a validating device only.
        ListState state = ListState::Recording;
        bool passOpen = false;
        BoundPipeline pipeline;   // Cleared by `beginRenderPass` and `endRenderPass`.
        size_t passScopeBase = 0; // Open scopes when the open pass began.
        ArenaPage page;           // Null until the first upload.
        // The list's last scope label copied into this frame's arena, shared by a scope of the same text.
        std::string_view scopeCopy;
        std::byte* cursor = nullptr; // The bump cursor; takes no lock.
        std::byte* limit = nullptr;  // One past the last byte of `page` an `Addr` reaches.
        RecordCounts counts;         // Folded into the device at submission.
    };

    // A backend's `CommandListImpl` embeds one. Each field declared here owns memory a recycled list keeps, and
    // `beginList` clears it by name, so a field added here is added there too.
    struct CommandListCore : ListFields
    {
        PassState pass;           // Meaningful while `passOpen`.
        std::vector<bool> scopes; // Innermost last: true when the scope began inside a render pass.
        std::string label;
        std::vector<ScopeRecord> scopeLog;           // In begin order, for `Device::scopeTimings`.
        std::string scopeCopySource;                 // The text of `scopeCopy`, in ordinary memory, to compare against.
        std::vector<ResourceAccess> pendingAccesses; // Not yet consumed by a draw or dispatch; validating devices only.
        TrackerLog trackerLog;                       // Validating devices only, in Debug.
    };

    // The tracker names each event's command by this count. Every recording entry the tracker may see calls it right
    // after `checkRecording`. Nothing in Release.
    inline void noteCommand([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        ++list.trackerLog.command;
#endif
    }

    // A plain counter on the list: no lock, no atomic. Only a backend calls it, at each native call it records.
    inline void noteNativeCall(CommandListCore& list)
    {
        ++list.counts.nativeRecordCalls;
    }

    // Each backend defines it.
    CommandListCore& coreOf(CommandList list);

    // A backend calls it first in `Device::acquireCommandList`.
    void checkAcquire(const DeviceCore& core);

    // Resets every field while keeping the capacity of its containers, and counts the list as unsubmitted for
    // VU-rojo-device-0005. The pending accesses and the tracker log, which only Debug-tier code writes, are cleared in
    // Debug builds only.
    void beginList(CommandListCore& list, DeviceCore& core, QueueType type, std::string_view label);

    // Every field to its default, the color formats keeping their capacity.
    void resetPass(PassState& pass);

    // VU-rojo-cmd-0063: the one check every recording entry but `end` calls first, except `allocateFrameData`, which
    // checks the upload's own rules before it.
    void checkRecording(const CommandListCore& list);

    void endList(CommandListCore& list);

    // A backend calls it first in `Queue::submit`.
    void submitLists(DeviceCore& core, QueueType type, std::span<const CommandList> lists);

    struct ArenaAllocation
    {
        Buffer page;
        uint64_t offset; // A multiple of the requested alignment.
        std::byte* memory;
        // Meaningful only where `ArenaPage::address` is: on Null, whose pages have none, it holds just the offset.
        uint64_t address;
    };

    // Stamps the page's record with the list's frame for VU-rojo-buffer-0020 in Debug, and sets the list's limit to the
    // smaller of the page's size and `Limits::maxAddrOffset`. The claim is the one place Release recording takes a lock
    // (ADR 0020), so it stays out of line.
    void claimArenaPage(DeviceCore& core, CommandListCore& list, uint64_t size, ArenaPageSource source, void* context);

    // Every arena page's host memory has this alignment, so rounding the cursor up to an alignment up to this one
    // rounds the page offset up to it too.
    inline constexpr uint32_t kArenaPageAlignment = 256;

    // Zero before the list has a page.
    inline uint64_t pageUsedBytes(const CommandListCore& list)
    {
        return static_cast<uint64_t>(list.cursor - list.page.host);
    }

    // The slow path of `bumpArenaPage`, out of line. An upload larger than the limit allows lands at offset zero of a
    // new page, past that page's limit, so the next upload claims again.
    ArenaAllocation bumpArenaPageSlow(DeviceCore& core, CommandListCore& list, uint64_t size, uint32_t alignment, ArenaPageSource source, void* context);

    // Only claiming takes a lock; the bump into a page with room, at an alignment up to `kArenaPageAlignment`, is
    // inline and makes no call. It checks nothing: `allocateFrameData` and the scope recorder call it after their own
    // checks.
    inline ArenaAllocation bumpArenaPage(DeviceCore& core, CommandListCore& list, uint64_t size, uint32_t alignment, ArenaPageSource source, void* context)
    {
        const uintptr_t mask = uintptr_t{ alignment } - 1;
        std::byte* const place = reinterpret_cast<std::byte*>((reinterpret_cast<uintptr_t>(list.cursor) + mask) & ~mask);
        if (alignment > kArenaPageAlignment || list.limit - place <= static_cast<std::ptrdiff_t>(size)) [[unlikely]]
        {
            return bumpArenaPageSlow(core, list, size, alignment, source, context);
        }
        list.cursor = place + size;
        const uint64_t offset = static_cast<uint64_t>(place - list.page.host);
        return ArenaAllocation{ list.page.buffer, offset, place, list.page.address + offset };
    }

    // The checks of `allocateFrameData`, in Debug builds only.
    void checkFrameDataChecked(const DeviceCore& core, const CommandListCore& list, std::span<const std::byte> bytes, uint32_t alignment);

    // The upload's own rules come before VU-rojo-cmd-0063 on purpose: a list used after its frame ended is a submitted
    // list, and the closed frame (VU-rojo-buffer-0017) is what the caller got wrong. The backend copies `bytes` to `memory`
    // and encodes the address. Inline, so a Release upload into a page with room makes no call.
    inline ArenaAllocation allocateFrameData(
        DeviceCore& core, CommandListCore& list, std::span<const std::byte> bytes, uint32_t alignment, ArenaPageSource source, void* context
    )
    {
#ifndef NDEBUG
        checkFrameDataChecked(core, list, bytes, alignment);
#endif
        ++list.counts.frameDataUploads;
        list.counts.frameArenaBytes += bytes.size();
        return bumpArenaPage(core, list, bytes.size(), alignment, source, context);
    }

    void checkSameDevice(const DeviceCore& owner, const DeviceCore& other, std::string_view kind);
} // namespace rojoRHI::core
