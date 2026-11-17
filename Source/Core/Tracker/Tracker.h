//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.h
/// @brief Declares the validation tracker's state of a device, its mutex, the replay of submitted lists and the device hooks.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Aliasing.h"
#include "Core/Tracker/ByteSegments.h"
#include "Core/Tracker/Clocks.h"
#include "Core/Tracker/FirstUse.h"
#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/Tiles.h"
#include "Core/Tracker/TrackerLog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rojoRHI::core {
    class DeviceCore;

    // Owns nothing on a device without validation and in Release builds.
    using TrackerLock = std::unique_lock<std::mutex>;

    // Per device: the layout and hazard state of every followed texture subresource and buffer byte range, per queue; the
    // uses of placed resources' heap bytes and of sparse tiles; and the clocks that order the queues. Lists record without
    // it, and `Queue::submit` replays their logs in queue submission order under its mutex, so queues submitting on several
    // threads see one order. A member with a `held` parameter requires the lock; every other public member takes it.
    class Tracker
    {
      public:
        // `Queue::submit` holds it from its wait check through replay to publishing its signals.
        TrackerLock lock();

        // `acquired` marks a swapchain image whose first barrier must start from `Undefined` (VU-rojo-present-0014). A placed
        // texture inherits the uses other resources already made of its heap bytes; a dedicated one passes a null heap.
        void addTexture(
            Texture texture,
            std::string_view label,
            uint32_t mips,
            uint32_t layers,
            Layout initial,
            bool acquired,
            Heap heap,
            uint64_t heapOffset,
            uint64_t heapSize,
            bool sparse
        );

        void removeTexture(Texture texture);

        std::optional<Layout> layoutOf(Texture texture, uint32_t mip, uint32_t layer);

        // A dedicated buffer passes a null heap. A `sparse` resource's tile uses and mapping changes are followed per queue
        // (VU-rojo-sparse-0013).
        void addBuffer(Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse);

        void removeBuffer(Buffer buffer);

        void removeHeap(Heap heap);

        std::optional<size_t> segmentCount(Buffer buffer);

        std::optional<size_t> fenceValueCount(Fence fence);

        std::optional<uint64_t> lastMapping(Texture texture, uint32_t mip, uint32_t layer, QueueType queue);

        std::optional<size_t> aliasMarkCount(Texture texture);

        std::optional<size_t> heapSegmentCount(Heap heap);

        // For a range spanning several segments, the worst order: `Missing`, then `Unordered`, `Ordered`, `None`. An unfollowed
        // buffer, such as a frame-arena page, has no write. For checks that report their own rule ahead of VU-rojo-sync-0021
        // and VU-rojo-sync-0024.
        WriteOrder lastWriteOrder(const TrackerLock& held, QueueType queue, Buffer buffer, uint64_t offset, uint64_t size, Scope use);

        // Replays in recording order and terminates at the first violation.
        void replay(const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list);

        // Every subresource must be in `Present` as the graphics queue sees it (VU-rojo-present-0018).
        void checkPresent(const TrackerLock& held, Texture image);

        // Orders this submission and every later one on `queue` after `value`. A value at most the fence's initial value orders
        // nothing.
        void wait(const TrackerLock& held, QueueType queue, Fence fence, uint64_t value);

        // Takes the next position on the queue, so its signals and a frame's join cover it even when it has no lists.
        void submitted(const TrackerLock& held, QueueType queue);

        // Each use of a changed tile on another queue must be covered by `queue`'s clock or the host clock
        // (VU-rojo-sparse-0013). The changes then take the next position on the queue, and mapped tiles are marked new,
        // unmapped ones cleared (VU-rojo-sparse-0014).
        void mapTiles(const TrackerLock& held, QueueType queue, std::span<const TileChange> changes);

        // The value records the queue's running clock.
        void signal(const TrackerLock& held, QueueType queue, Fence fence, uint64_t initial, uint64_t value);

        // From `Device::wait` or `Device::completedValue`: the host clock joins what the value orders.
        void observe(const TrackerLock& held, Fence fence, uint64_t value);

        // The graphics clock must cover the frame's last compute and copy submissions (VU-rojo-device-0006); the frame slot's
        // internal fence then orders what that clock does now.
        void endFrame(const TrackerLock& held, uint64_t frame);

        // `retired` is the frame whose slot was retired by waiting on its internal fence, zero for none: the host clock joins the
        // graphics clock its `endFrame` recorded.
        void beginFrame(const TrackerLock& held, uint64_t retired);

        // The first of the compute and copy queues whose last submission of the open frame the graphics clock does not cover.
        std::optional<QueueType> unjoined(const TrackerLock& held) const;

        std::optional<QueueType> unjoined();

        void removeFence(Fence fence);

        // The join of its submissions' clocks; its own component is the position of the last event there, a submission or a
        // later tile mapping change.
        Clock runningClock(const TrackerLock& held, QueueType queue) const;

        Clock runningClock(QueueType queue);

        Clock hostClock(const TrackerLock& held) const;

        Clock hostClock();

      private:
        // A creation, an acquire, a barrier that changed the layout or discarded, or another resource's use of the memory (which
        // leaves it `Undefined`). A queue sees the layout when the event is on it, precedes every submission, or is covered by
        // its submission's clock; otherwise the queues race (VU-rojo-queue-0005). A covered event on another queue hands the
        // subresource over (VU-rojo-sync-0025) unless it left it `Undefined`. An event with no queue is held by the queue of the
        // latest use since. A Copy list's first use or barrier sets a created `Undefined` subresource to `General` unless the
        // texture is placed, sparse or an acquired swapchain image (`newOnCopy`).
        struct LayoutEvent
        {
            Layout layout = Layout::Undefined;
            std::optional<QueueType> queue; // None for a creation or an acquire.
            uint64_t position = 0;
            std::optional<QueueType> lastUse; // With no `queue`, the queue of the latest use or barrier since.
        };

        struct TextureState
        {
            std::string label;
            uint32_t mips = 1;
            uint32_t layers = 1;
            std::vector<LayoutEvent> layouts; // At `mip * layers + layer`.
            std::vector<UnitHazards> hazards; // Indexed as `layouts`.
            bool acquired = false;            // A swapchain image with no barrier replayed since its acquire.
            Heap heap = {};
            uint64_t heapOffset = 0;
            uint64_t heapSize = 0;
            // Placed only: per subresource and queue, the latest use other resources made of each byte range of its memory since
            // the subresource's last event there, or since an event on another queue a clock orders after that use.
            std::vector<std::array<AliasMarks, 3>> aliases;
            // Sparse only, per subresource and queue.
            std::vector<std::array<TileEvents, 3>> tileUses;
            std::vector<std::array<TileEvents, 3>> tileMaps;
            // Sparse only: tiles mapped since the subresource's last barrier from `Undefined`, whose next event must be such a
            // barrier (VU-rojo-sparse-0014).
            std::vector<TileMarks> newTiles;
        };

        struct BufferState
        {
            std::string label;
            uint64_t size = 0;
            Heap heap = {};
            uint64_t heapOffset = 0;
            ByteSegments segments;
            bool sparse = false;
            std::array<ByteEvents, 3> byteUses;
            std::array<ByteEvents, 3> byteMaps;
            // Sparse only: byte ranges newly mapped, whose first use must follow a global barrier (VU-rojo-sparse-0014).
            ByteMarks newBytes;
        };

        struct HeapState
        {
            HeapSegments segments;
            std::vector<uint64_t> textures;
        };

        TextureState* locate(const TrackerLock& held, Texture texture);

        BufferState* locate(const TrackerLock& held, Buffer buffer);

        HeapState* locate(const TrackerLock& held, Heap heap);

        std::string describe(const TrackerLock& held, HeapOwner owner);

        // The bytes become `owner`'s on `queue`: every other texture placed over them must discard before its next event there,
        // and their subresources' latest layout event becomes this use, leaving them `Undefined` on every queue it is ordered
        // before. A `transition` notes its destination as `scope` and is a write whatever its accesses.
        void noteHeapUse(
            const TrackerLock& held,
            HeapState& heap,
            HeapOwner owner,
            uint64_t begin,
            uint64_t end,
            QueueType queue,
            uint64_t position,
            Scope scope,
            bool transition = false
        );

        // Against other resources' last use of the bytes on every other queue (VU-rojo-queue-0005): a pair with a write, a
        // `transition` counting as one, is ordered only when the use's submission clock or the host clock covers the other.
        void checkHeapQueues(
            const TrackerLock& held,
            const HeapState& heap,
            HeapOwner owner,
            std::string_view resource,
            uint64_t begin,
            uint64_t end,
            QueueType queue,
            Scope scope,
            std::string_view list,
            bool transition = false
        );

        // Checks on the queue (VU-rojo-sync-0009) and on every other queue (VU-rojo-queue-0005), then notes the use on the heap.
        // A use a fence or the CPU's observation orders needs no barrier. The buffer's own hazard state restarts over the
        // bytes that pass against another resource's use.
        void replayBufferAlias(const TrackerLock& held, BufferState& buffer, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        // The event follows every mark on `queue` and every mark elsewhere that `ordered` covers, so none of them owes a discard.
        static void followMarks(TextureState& texture, size_t index, QueueType queue, const Clock& ordered);

        // Nothing when the event is on another queue and `ordered` does not cover it: a race VU-rojo-queue-0005 reports.
        static std::optional<Layout> layoutOn(const LayoutEvent& event, QueueType queue, const Clock& ordered);

        // For the handoff rule (VU-rojo-sync-0025): with no queue on the event, the queue of the latest use or barrier since.
        static std::optional<QueueType> holderOf(const LayoutEvent& event);

        // A Copy list finds the subresource in `General` (09 `Layout`) when its creation left it `Undefined`, nothing reached it
        // since, and the texture is neither placed, sparse nor an acquired swapchain image.
        static bool newOnCopy(const TextureState& texture, const LayoutEvent& event);

        // Its own component is reached only through fences.
        Clock orderedBefore(QueueType queue) const;

        // An empty `box` means the whole subresource. `ordered` must cover every mapping change of its tiles on other queues
        // (VU-rojo-sparse-0013).
        void replayTileUse(
            TextureState& texture,
            size_t index,
            const std::optional<TexelBox>& box,
            QueueType queue,
            std::string_view list,
            uint64_t position,
            const Clock& ordered
        );

        // Reports the rule of the entry that made the read.
        void replayIndirect(const TrackerLock& held, const BufferState& buffer, const UseEvent& use, QueueType queue, std::string_view list);

        // Per queue type, the position below which no remembered use belongs to a command still to be replayed: the current
        // position on `queue`, one past the last elsewhere. Segments merge only below it.
        std::array<uint64_t, 3> mergeFloor(QueueType queue) const;

        // Report order: for a texture, the layout on the queue (VU-rojo-sync-0025 for a handover in a layout the queues do not
        // share, VU-rojo-sync-0006, the entry's own rule, VU-rojo-sync-0004), leaving an unordered event on another queue to
        // VU-rojo-queue-0005; then sparse mapping changes on other queues (VU-rojo-sparse-0013); then hazards on the queue
        // (VU-rojo-sync-0021, VU-rojo-sync-0024) and across queues (VU-rojo-queue-0005); then newly mapped tiles, which a
        // texture's discard or a buffer's global barrier must order it after (VU-rojo-sparse-0014).
        void replayUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        void replayTextureHazards(TextureState& texture, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        void replayBufferUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        // Also reaches every newly mapped byte range it follows (VU-rojo-sparse-0014).
        void replayGlobal(const GlobalBarrier& barrier, QueueType queue, uint64_t position);

        void replayBuffer(const TrackerLock& held, const BufferBarrier& barrier, QueueType queue, uint64_t position);

        // Report order: the old layout (VU-rojo-present-0014; VU-rojo-sync-0025 for a non-discarding handover from another
        // queue; VU-rojo-sync-0013); for a transition, its source against earlier uses (VU-rojo-sync-0024), then as a write of
        // every tile against other queues' mapping changes (VU-rojo-sparse-0013), uses and heap bytes (VU-rojo-queue-0005); then
        // newly mapped tiles, which only a discard from `Undefined` may reach and clears (VU-rojo-sparse-0014). A barrier that
        // keeps the layout sets no layout event.
        void replayTexture(const TrackerLock& held, const TextureBarrier& barrier, QueueType queue, std::string_view list, uint64_t position);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, TextureState> m_textures;
        std::unordered_map<uint64_t, BufferState> m_buffers;
        std::unordered_map<uint64_t, HeapState> m_heaps;
        // Each replayed command, each submission once replayed and each `mapTiles` call takes the next position, so a clock
        // that reaches a position covers everything before it on that queue.
        std::array<uint64_t, 3> m_positions = {};
        // Per queue, the join of every fence value waited on: its own component is reached only through a fence.
        std::array<Clock, 3> m_waited = {};
        // From `Device::wait` returning true, `completedValue` and `beginFrame` retiring a slot.
        Clock m_host = {};
        std::unordered_map<uint64_t, FenceClocks> m_fences;
        std::map<uint64_t, Clock> m_frames; // Until its slot retires.
        // A submission above it is the frame's.
        std::array<uint64_t, 3> m_frameStart = {};
        std::array<uint64_t, 3> m_submitted = {};
    };

    // The Debug bodies of the inline hooks below, which compile to nothing in Release.
    TrackerLock lockTrackerChecked(DeviceCore& core);

    void replayListsChecked(
        DeviceCore& core,
        QueueType type,
        std::span<const CommandList> lists,
        std::span<const FenceValue> wait,
        std::span<const FenceValue> signal,
        const TrackerLock& held
    );

    void observeFenceChecked(DeviceCore& core, Fence fence, uint64_t value);

    void endFrameClockChecked(DeviceCore& core, uint64_t frame);

    void beginFrameClockChecked(DeviceCore& core, uint64_t retired);

    void mapTileChangesChecked(DeviceCore& core, QueueType queue, std::span<const TileChange> changes);

    void forgetFenceChecked(DeviceCore& core, Fence fence);

    void checkPresentLayoutChecked(DeviceCore& core, Texture image);

    void trackTextureChecked(
        DeviceCore& core,
        Texture texture,
        std::string_view label,
        uint32_t mips,
        uint32_t layers,
        Layout initial,
        bool acquired,
        Heap heap,
        uint64_t heapOffset,
        uint64_t heapSize,
        bool sparse
    );

    void forgetTextureChecked(DeviceCore& core, Texture texture);

    void trackBufferChecked(DeviceCore& core, Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse);

    void forgetBufferChecked(DeviceCore& core, Buffer buffer);

    void forgetHeapChecked(DeviceCore& core, Heap heap);

    // `Queue::submit` takes it before `checkSubmit` and keeps it until `recordSignals` returns.
    inline TrackerLock lockTracker([[maybe_unused]] DeviceCore& core)
    {
#ifndef NDEBUG
        return lockTrackerChecked(core);
#else
        return TrackerLock{};
#endif
    }

    // Called after `checkSubmit` and before `recordSignals`, so a value is never published without its clock.
    inline void replayLists(
        [[maybe_unused]] DeviceCore& core,
        [[maybe_unused]] QueueType type,
        [[maybe_unused]] std::span<const CommandList> lists,
        [[maybe_unused]] std::span<const FenceValue> wait,
        [[maybe_unused]] std::span<const FenceValue> signal,
        [[maybe_unused]] const TrackerLock& held
    )
    {
#ifndef NDEBUG
        replayListsChecked(core, type, lists, wait, signal, held);
#endif
    }

    // The fence calls run it after reading the fence's record, never holding its pool lock.
    inline void observeFence([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Fence fence, [[maybe_unused]] uint64_t value)
    {
#ifndef NDEBUG
        observeFenceChecked(core, fence, value);
#endif
    }

    inline void endFrameClock([[maybe_unused]] DeviceCore& core, [[maybe_unused]] uint64_t frame)
    {
#ifndef NDEBUG
        endFrameClockChecked(core, frame);
#endif
    }

    inline void beginFrameClock([[maybe_unused]] DeviceCore& core, [[maybe_unused]] uint64_t retired)
    {
#ifndef NDEBUG
        beginFrameClockChecked(core, retired);
#endif
    }

    // The caller reads the records it builds `changes` from first, never holding the tracker's lock.
    inline void mapTileChanges([[maybe_unused]] DeviceCore& core, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::span<const TileChange> changes)
    {
#ifndef NDEBUG
        mapTileChangesChecked(core, queue, changes);
#endif
    }

    inline void forgetFence([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Fence fence)
    {
#ifndef NDEBUG
        forgetFenceChecked(core, fence);
#endif
    }

    inline void trackTexture(
        [[maybe_unused]] DeviceCore& core,
        [[maybe_unused]] Texture texture,
        [[maybe_unused]] std::string_view label,
        [[maybe_unused]] uint32_t mips,
        [[maybe_unused]] uint32_t layers,
        [[maybe_unused]] Layout initial,
        [[maybe_unused]] bool acquired,
        [[maybe_unused]] Heap heap,
        [[maybe_unused]] uint64_t heapOffset,
        [[maybe_unused]] uint64_t heapSize,
        [[maybe_unused]] bool sparse
    )
    {
#ifndef NDEBUG
        trackTextureChecked(core, texture, label, mips, layers, initial, acquired, heap, heapOffset, heapSize, sparse);
#endif
    }

    inline void checkPresentLayout([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Texture image)
    {
#ifndef NDEBUG
        checkPresentLayoutChecked(core, image);
#endif
    }

    inline void forgetTexture([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Texture texture)
    {
#ifndef NDEBUG
        forgetTextureChecked(core, texture);
#endif
    }

    // A frame-arena page is not followed: the CPU writes it and the GPU only reads it within its frame.
    inline void trackBuffer(
        [[maybe_unused]] DeviceCore& core,
        [[maybe_unused]] Buffer buffer,
        [[maybe_unused]] std::string_view label,
        [[maybe_unused]] uint64_t size,
        [[maybe_unused]] Heap heap,
        [[maybe_unused]] uint64_t heapOffset,
        [[maybe_unused]] bool sparse
    )
    {
#ifndef NDEBUG
        trackBufferChecked(core, buffer, label, size, heap, heapOffset, sparse);
#endif
    }

    inline void forgetBuffer([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Buffer buffer)
    {
#ifndef NDEBUG
        forgetBufferChecked(core, buffer);
#endif
    }

    inline void forgetHeap([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Heap heap)
    {
#ifndef NDEBUG
        forgetHeapChecked(core, heap);
#endif
    }
} // namespace rojoRHI::core
