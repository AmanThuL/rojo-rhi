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
    /// Where a command list is in its life: recording from acquisition, ended by `CommandList::end`, then submitted.
    enum class ListState : uint8_t
    {
        Recording, ///< Acquired and not yet ended.
        Ended,     ///< Ended and not yet submitted.
        Submitted  ///< Handed to a queue; it records nothing further.
    };

    /// The render pass open on a list: what `setPipeline` and the dynamic-state setters compare against.
    struct PassState
    {
        std::vector<Format> colors;             ///< The format of each color attachment, in order.
        Format depthFormat = Format::Undefined; ///< The depth attachment's format; `Undefined` for none.
        uint32_t samples = 1;                   ///< Samples per texel of every attachment other than a resolve target.
        Rect2D area;                            ///< The area the pass renders to.
        bool viewportSet = false;               ///< `setViewport` was recorded since the pass began.
        bool scissorSet = false;                ///< `setScissor` was recorded since the pass began.
    };

    /// The pipeline bound on a list: the fields the dynamic-state and draw checks compare against.
    struct BoundPipeline
    {
        bool bound = false;                                  ///< A pipeline is bound.
        PipelineKind kind = PipelineKind::Graphics;          ///< Graphics or compute.
        uint32_t stages = 0;                                 ///< Bit `1 << ShaderStage` for each stage the pipeline has.
        PrimitiveClass primitive = PrimitiveClass::Triangle; ///< The topology class of a vertex pipeline.
        bool depthBias = false;                              ///< The pipeline was created with `RasterState::depthBias`.
        uint32_t blendMask = 0;                              ///< Bit `i` is set when color target `i` blends.
        bool depthWrite = false;                             ///< The pipeline writes depth.
        bool stencilWrite = false;                           ///< The pipeline has a stencil operation other than `Keep`.
    };

    /// What the core keeps about one command list: the state machine the recording rules read. A backend's `CommandListImpl`
    /// embeds one.
    struct CommandListCore
    {
        DeviceCore* device = nullptr;         ///< The device that acquired the list.
        QueueType type = QueueType::Graphics; ///< The queue type the list was acquired for.
        uint64_t frame = 0;                   ///< The frame the list was acquired in.
        Addr root = {};                       ///< The root `setRoot` bound; null until one is.
        Buffer rootBuffer = {};               ///< The buffer `root` resolved to; written in Debug on a validating device only.
        ListState state = ListState::Recording;
        bool passOpen = false;                       ///< A render pass is open on the list.
        PassState pass;                              ///< The open pass; meaningful while `passOpen`.
        BoundPipeline pipeline;                      ///< The bound pipeline; cleared by `beginRenderPass` and `endRenderPass`.
        std::vector<bool> scopes;                    ///< One entry per open scope, innermost last: true when it began inside a render pass.
        size_t passScopeBase = 0;                    ///< The number of open scopes when the open pass began.
        std::string label;                           ///< The label the list was acquired with.
        ArenaPage page;                              ///< The arena page the list uploads into; null until its first upload.
        uint64_t pageUsed = 0;                       ///< Bytes of `page` the list has used: its bump cursor, which takes no lock.
        std::vector<ScopeRecord> scopeLog;           ///< The scopes recorded on the list in the order they began, for `Device::scopeTimings`.
        std::vector<ResourceAccess> pendingAccesses; ///< Declared accesses no draw or dispatch has consumed; kept only on a validating device.
        TrackerLog trackerLog;                       ///< The uses and barriers the list recorded; kept only on a validating device, in Debug.
        RecordCounts counts;                         ///< What the list recorded for `Device::counters`; folded into the device at submission.
    };

    /// Counts one recording entry whose Metal 4 mapping row names a native call, on the list's own plain counter: no lock and
    /// no atomic operation. Every such entry calls it right after `checkRecording`.
    inline void noteNativeCall(CommandListCore& list)
    {
        ++list.counts.nativeRecordCalls;
    }

    /// Returns the core of the command list `list` wraps (VU-rojo-core-0004). Each backend defines it, as it does for a
    /// device.
    CommandListCore& coreOf(CommandList list);

    /// Checks that a frame is open (VU-rojo-cmd-0001); a backend calls it first in `Device::acquireCommandList`.
    void checkAcquire(const DeviceCore& core);

    /// Starts `list`, which a backend took from its pool, as a recording list of `type` in the open frame: it checks the label
    /// (VU-rojo-core-0006, VU-rojo-core-0007), resets every field while keeping the capacity of its containers, and counts
    /// the list as unsubmitted for VU-rojo-device-0005.
    void beginList(CommandListCore& list, DeviceCore& core, QueueType type, std::string_view label);

    /// Checks that a command other than `end` is recorded on a list that is recording (VU-rojo-cmd-0063): the one site every
    /// recording entry calls first, except `allocateFrameData`, which checks the upload's own rules before it.
    void checkRecording(const CommandListCore& list);

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

    /// Reserves `size` bytes at `alignment` in `list`'s own page, claiming a page when it has none or the bytes do not fit,
    /// through `source`. Only claiming takes a lock. It checks nothing: `allocateFrameData` and the scope recorder call it
    /// after their own checks.
    ArenaAllocation bumpArenaPage(DeviceCore& core, CommandListCore& list, uint64_t size, uint32_t alignment, ArenaPageSource source, void* context);

    /// Checks an upload of `bytes` at `alignment` (VU-rojo-buffer-0015 to 0017, then VU-rojo-cmd-0063) and bumps the cursor of
    /// `list`'s own page; only a list with no page, or whose page is full, claims a page from the arena, which takes a lock
    /// and may grow it through `source`. The backend copies `bytes` to `memory` and encodes the address. The upload's own
    /// rules come before VU-rojo-cmd-0063 on purpose: a list used after its frame ended is a submitted list, and the closed
    /// frame (VU-rojo-buffer-0017) is what the caller got wrong.
    ArenaAllocation allocateFrameData(
        DeviceCore& core, CommandListCore& list, std::span<const std::byte> bytes, uint32_t alignment, ArenaPageSource source, void* context
    );

    /// Issues VU-rojo-core-0005 when `other`, the device an object of kind `kind` belongs to, is not `owner`.
    void checkSameDevice(const DeviceCore& owner, const DeviceCore& other, std::string_view kind);
} // namespace rojoRHI::core
