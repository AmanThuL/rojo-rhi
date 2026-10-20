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

    /// A hold on a tracker's mutex; an empty lock, which owns nothing, on a device without validation and in Release builds.
    using TrackerLock = std::unique_lock<std::mutex>;

    /// The state the validation tracker of one device judges submitted work against: the latest layout event, with its queue,
    /// of each subresource of every live texture, the hazard state per queue of every texture subresource and of the byte segments of every live buffer, per
    /// heap holding placed resources the resource that last used each byte range on each queue, per sparse resource the latest
    /// use and mapping change of its tiles on each queue, and the clocks that order the queues: per queue the clocks of the
    /// fence values it waited on, per fence the clock of each value submitted for signal, the host clock of what the CPU
    /// observed complete, and the position of each queue's last submission against the open frame's start.
    /// Lists record events without it; `Queue::submit` replays their logs into it in queue submission order. Every member
    /// takes or requires its mutex, so queues submitting on several threads see one order.
    class Tracker
    {
      public:
        /// Locks the state. `Queue::submit` holds the lock from its wait check through replay to publishing its signals.
        TrackerLock lock();

        /// Starts following `texture`, labelled `label`, of `mips` mips and `layers` layers, with every subresource in
        /// `initial`. `acquired` marks a swapchain image whose first barrier must start from `Undefined` (VU-rojo-present-0014).
        /// A placed texture names its `heap` and the `heapSize` bytes from `heapOffset` it occupies there; it follows the uses
        /// other resources already made of those bytes. A dedicated one passes a null heap. A `sparse` texture also has the uses
        /// and mapping changes of its tiles followed per queue (VU-rojo-sparse-0013). Takes the lock.
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

        /// Stops following `texture`, destroyed or presented; a texture the tracker does not follow is ignored. Takes the lock.
        void removeTexture(Texture texture);

        /// Returns the layout the latest layout event of mip `mip` and layer `layer` of `texture` set, on whichever queue, or
        /// nothing when the tracker does not follow it. Takes the lock.
        std::optional<Layout> layoutOf(Texture texture, uint32_t mip, uint32_t layer);

        /// Starts following `buffer`, labelled `label`, of `size` bytes, with no hazard on any queue. A placed buffer names its
        /// `heap` and the byte offset `heapOffset` it occupies there; a dedicated one passes a null heap. A `sparse` buffer also
        /// has the uses and mapping changes of its byte ranges followed per queue (VU-rojo-sparse-0013). Takes the lock.
        void addBuffer(Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse);

        /// Stops following `buffer`, destroyed; a buffer the tracker does not follow is ignored. Takes the lock.
        void removeBuffer(Buffer buffer);

        /// Forgets the uses made of the bytes of `heap`, destroyed with no resource placed in it; a heap no placed resource was
        /// followed in is ignored. Takes the lock.
        void removeHeap(Heap heap);

        /// Returns the number of byte segments the tracker keeps for `buffer`, or nothing when it does not follow it. Takes the
        /// lock.
        std::optional<size_t> segmentCount(Buffer buffer);

        /// Returns the number of values the tracker keeps the clocks of for `fence`, or nothing when no submission signalled it.
        /// Takes the lock.
        std::optional<size_t> fenceValueCount(Fence fence);

        /// Returns the position on `queue` of the latest mapping change of any tile of mip `mip` and layer `layer` of `texture`,
        /// zero for none, or nothing when the tracker does not follow it as a sparse texture. Takes the lock.
        std::optional<uint64_t> lastMapping(Texture texture, uint32_t mip, uint32_t layer, QueueType queue);

        /// Returns the number of alias marks `texture` holds over every subresource and queue, or nothing when the tracker does
        /// not follow it. Takes the lock.
        std::optional<size_t> aliasMarkCount(Texture texture);

        /// Returns the number of byte segments the tracker keeps for `heap`, or nothing when no placed resource of it is
        /// followed. Takes the lock.
        std::optional<size_t> heapSegmentCount(Heap heap);

        /// Returns how the last write on `queue` to `size` bytes from `offset` of `buffer` is ordered before a use of `use`: for
        /// the bytes of several segments, the worst of them, `Missing` before `Unordered` before `Ordered` before `None`. A
        /// buffer the tracker does not follow, such as a frame-arena page, has no write. A check that reports its own rule ahead
        /// of VU-rojo-sync-0021 and VU-rojo-sync-0024 calls it during replay. `held` must be a lock `lock` returned.
        WriteOrder lastWriteOrder(const TrackerLock& held, QueueType queue, Buffer buffer, uint64_t offset, uint64_t size, Scope use);

        /// Replays `log`, the log of the list labelled `list` submitted to a queue of `queue`, event by event in recording order,
        /// and terminates at the first violation. `held` must be a lock `lock` returned.
        void replay(const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list);

        /// Checks that every subresource of `image`, a swapchain image presented to the graphics queue, is in `Layout::Present`
        /// as that queue sees it: the latest layout event set `Present` and is on the graphics queue or covered by its clock
        /// (VU-rojo-present-0018); an image the tracker does not follow is ignored. `held` must be a lock `lock` returned.
        void checkPresent(const TrackerLock& held, Texture image);

        /// Orders the submission to `queue` about to be replayed, and every later one there, after `value` of `fence`, which it
        /// waits on: the queue's clock joins what reaching the value orders. A value at most the fence's initial value orders
        /// nothing. `held` must be a lock `lock` returned.
        void wait(const TrackerLock& held, QueueType queue, Fence fence, uint64_t value);

        /// Notes that a submission to `queue`, its waits joined and its lists replayed, is complete as a submission: it takes the
        /// next position on the queue, so its signals, and a frame's join, cover it even when it has no lists.
        void submitted(const TrackerLock& held, QueueType queue);

        /// Checks `changes`, the mapping changes of one `Queue::mapTiles` call on `queue`, against the uses of their tiles on
        /// every other queue: each use must be covered by the queue's clock, through its earlier submissions' waits, or by the
        /// host clock (VU-rojo-sparse-0013). Then notes them as one event at the next position on the queue, which later uses on
        /// other queues must be ordered after, and marks the tiles each change maps as new, or clears the marks of those it
        /// unmaps (VU-rojo-sparse-0014). A resource the tracker does not follow as sparse is ignored. `held` must be a lock
        /// `lock` returned.
        void mapTiles(const TrackerLock& held, QueueType queue, std::span<const TileChange> changes);

        /// Notes that the submission to `queue` just replayed signals `value` of `fence`, created at `initial`: the value records
        /// the queue's running clock. `held` must be a lock `lock` returned.
        void signal(const TrackerLock& held, QueueType queue, Fence fence, uint64_t initial, uint64_t value);

        /// Notes that the CPU observed `fence` reach `value`, through `Device::wait` or `Device::completedValue`: the host clock
        /// joins what reaching the value orders, so every later check finds that work complete. `held` must be a lock `lock`
        /// returned.
        void observe(const TrackerLock& held, Fence fence, uint64_t value);

        /// Checks that frame `frame` may end: the graphics queue's running clock covers the last compute and copy submissions of
        /// the frame (VU-rojo-device-0006). Then notes that its slot's internal fence, signalled on the graphics queue, orders
        /// what that clock does now. `held` must be a lock `lock` returned.
        void endFrame(const TrackerLock& held, uint64_t frame);

        /// Notes that `beginFrame` opened a frame: submissions from now on are the frame's. When it retired the slot of frame
        /// `retired`, zero for none, waiting on its internal fence, the host clock joins the graphics clock that frame's
        /// `endFrame` recorded. `held` must be a lock `lock` returned.
        void beginFrame(const TrackerLock& held, uint64_t retired);

        /// Returns the first of the compute and copy queues whose last submission of the open frame the graphics queue's running
        /// clock does not cover, or nothing when the graphics submissions of the frame, together, wait on every such
        /// submission. `held` must be a lock `lock` returned.
        std::optional<QueueType> unjoined(const TrackerLock& held) const;

        /// Returns the queue `unjoined` returns, as the overload with a lock does. Takes the lock.
        std::optional<QueueType> unjoined();

        /// Forgets the clocks of the values of `fence`, destroyed; a fence no submission signalled is ignored. Takes the lock.
        void removeFence(Fence fence);

        /// Returns the running clock of `queue`: the join of its submissions' clocks, whose own component is the position of the
        /// last event there, its last submission or a later tile mapping change. A signal records it. `held` must be a lock `lock`
        /// returned.
        Clock runningClock(const TrackerLock& held, QueueType queue) const;

        /// Returns the running clock of `queue`, as the overload with a lock does. Takes the lock.
        Clock runningClock(QueueType queue);

        /// Returns the host clock: what the CPU has observed complete through fences and retired frame slots. `held` must be a
        /// lock `lock` returned.
        Clock hostClock(const TrackerLock& held) const;

        /// Returns the host clock, as the overload with a lock does. Takes the lock.
        Clock hostClock();

      private:
        /// The latest event that set the layout of one subresource: its creation, an acquire, a texture barrier that changed
        /// the layout or discarded, or another resource's use of its memory, which leaves it `Undefined`. A queue sees the
        /// layout it set when the event is on that queue, precedes every submission, or is covered by the clock of the queue's
        /// submission; otherwise the two queues race (VU-rojo-queue-0005). An event on another queue that the clock covers
        /// hands the subresource over (VU-rojo-sync-0025), unless it left the subresource `Undefined`. An event with no queue
        /// is held by the queue of the latest use since, and a use on another queue takes it over in the event's layout.
        struct LayoutEvent
        {
            Layout layout = Layout::Undefined; ///< The layout the event set.
            std::optional<QueueType> queue;    ///< The queue of a barrier; none for a creation or an acquire.
            uint64_t position = 0;             ///< The barrier's position on its queue.
            std::optional<QueueType> lastUse;  ///< With no `queue`, the queue of the latest use or barrier since the event.
        };

        /// The tracker's view of one texture.
        struct TextureState
        {
            std::string label;                ///< The texture's label, for reports.
            uint32_t mips = 1;                ///< Mip levels.
            uint32_t layers = 1;              ///< Array layers.
            std::vector<LayoutEvent> layouts; ///< The latest layout event of each subresource, at `mip * layers + layer`.
            std::vector<UnitHazards> hazards; ///< The hazard state of each subresource, indexed as `layouts`.
            bool acquired = false;            ///< A swapchain image no barrier has been replayed on since its acquire.
            Heap heap = {};                   ///< The heap the texture is placed in; null for a dedicated allocation.
            uint64_t heapOffset = 0;          ///< The byte offset of the texture in `heap`.
            uint64_t heapSize = 0;            ///< The bytes the texture occupies in `heap` from `heapOffset`.
            /// For a placed texture, per subresource indexed as `layouts` and per queue type, the latest use other resources made
            /// of each byte range of its memory since the subresource's last event on that queue, or since an event on another
            /// queue that a clock orders after that use; empty for a dedicated texture.
            std::vector<std::array<AliasMarks, 3>> aliases;
            /// For a sparse texture, per subresource indexed as `layouts` and per queue type, the latest uses of its tiles; empty
            /// for a texture that is not sparse.
            std::vector<std::array<TileEvents, 3>> tileUses;
            /// For a sparse texture, per subresource indexed as `layouts` and per queue type, the latest mapping changes of its
            /// tiles; empty for a texture that is not sparse.
            std::vector<std::array<TileEvents, 3>> tileMaps;
            /// For a sparse texture, per subresource indexed as `layouts`, the tiles newly mapped since its last texture barrier
            /// from `Undefined`, whose next event must be such a barrier (VU-rojo-sparse-0014); empty for a texture that is not
            /// sparse.
            std::vector<TileMarks> newTiles;
        };

        /// The tracker's view of one buffer.
        struct BufferState
        {
            std::string label;                  ///< The buffer's label, for reports.
            uint64_t size = 0;                  ///< Bytes.
            Heap heap = {};                     ///< The heap the buffer is placed in; null for a dedicated allocation.
            uint64_t heapOffset = 0;            ///< The byte offset of the buffer in `heap`; its heap range runs `size` bytes from there.
            ByteSegments segments;              ///< The hazard state of its bytes.
            bool sparse = false;                ///< The buffer is sparse: the uses and mapping changes of its tiles are followed.
            std::array<ByteEvents, 3> byteUses; ///< For a sparse buffer, per queue type, the latest use of each byte range.
            std::array<ByteEvents, 3> byteMaps; ///< For a sparse buffer, per queue type, the latest mapping change of each range.
            /// For a sparse buffer, the byte ranges newly mapped whose first use must follow a global barrier (VU-rojo-sparse-0014).
            ByteMarks newBytes;
        };

        /// The tracker's view of one heap that holds or held a followed placed resource.
        struct HeapState
        {
            HeapSegments segments;          ///< The last use of each byte range on each queue, and the resource that made it.
            std::vector<uint64_t> textures; ///< The handle bits of the followed textures placed in the heap.
        };

        /// Returns the state of `texture`, or null when the tracker does not follow it. Unlike the public lookups it takes no
        /// lock, so the replay, which holds one, may call it.
        TextureState* locate(const TrackerLock& held, Texture texture);

        /// Returns the state of `buffer`, or null when the tracker does not follow it, as the texture lookup does.
        BufferState* locate(const TrackerLock& held, Buffer buffer);

        /// Returns the state of `heap`, or null when no placed resource of it was followed, as the texture lookup does.
        HeapState* locate(const TrackerLock& held, Heap heap);

        /// Returns the resource `owner` names for reports: its kind and label, or a destroyed resource's kind.
        std::string describe(const TrackerLock& held, HeapOwner owner);

        /// Notes the use `scope` by `owner` of the bytes `begin` to `end` of `heap` at `position` on `queue`: they become the
        /// owner's on that queue, and every other texture placed over them must discard before its next event there; their
        /// subresources' latest layout event becomes this use, which leaves them `Undefined` on every queue it is ordered before.
        /// A `transition`, a placed texture's barrier that changes the layout or discards, notes its destination as `scope` and
        /// is a write whatever its accesses.
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

        /// Checks `scope`, a use by `owner`, described as `resource`, of the bytes `begin` to `end` of `heap` on `queue`, against
        /// the last use another resource made of them on every other queue (VU-rojo-queue-0005): a pair at least one of which
        /// writes, a transition counting as a write, is ordered only when the clock of the use's submission, or the host clock, covers the other use. A
        /// `transition`, a texture barrier that changes the layout or discards, is a write whatever its accesses.
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

        /// Checks `use`, a use of the placed buffer `buffer`, at `position` on `queue`, against the last use by another resource of
        /// each heap byte range it covers, on the queue (VU-rojo-sync-0009) and on every other queue (VU-rojo-queue-0005), then
        /// notes it on the heap. A use a fence or the CPU's observation orders needs no barrier. The buffer's own hazard state
        /// on the queue starts afresh over the bytes that pass against another resource's use there.
        void replayBufferAlias(const TrackerLock& held, BufferState& buffer, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Notes that subresource `index` of `texture`, a placed one, had an event on `queue` that passed its marks there: it
        /// follows every mark on that queue, and every mark on another queue that `ordered`, the clock of the event's
        /// submission joined with the host clock, covers, so none of them owes a discard any more.
        static void followMarks(TextureState& texture, size_t index, QueueType queue, const Clock& ordered);

        /// Returns the layout a subresource whose latest layout event is `event` has on `queue`, for work ordered after
        /// `ordered`, the clock of its submission joined with the host clock: the event's layout when the event precedes every
        /// submission, is on `queue` or is covered by `ordered`; nothing when it is on another queue and not covered, a race
        /// VU-rojo-queue-0005 reports rather than a layout either queue sees.
        static std::optional<Layout> layoutOn(const LayoutEvent& event, QueueType queue, const Clock& ordered);

        /// Returns the queue that holds a subresource whose latest layout event is `event`, for the handoff rule
        /// (VU-rojo-sync-0025): the event's queue, or, for a creation or an acquire, the queue of the latest use or barrier
        /// since; none before the first.
        static std::optional<QueueType> holderOf(const LayoutEvent& event);

        /// Returns the clock that orders earlier work before the work replayed now on `queue`: the join of the clocks of the
        /// values its submissions waited on, so its own component is reached only through fences, and the host clock.
        Clock orderedBefore(QueueType queue) const;

        /// Checks the use, or the transition, at `position` on `queue` of the texels `box` of subresource `index` of `texture`,
        /// the whole subresource when `box` is empty, against the mapping changes of its tiles on every other queue: `ordered`,
        /// the clock of the use's submission joined with the host clock, must cover each (VU-rojo-sparse-0013). Then notes the
        /// use. A texture that is not sparse is ignored.
        void replayTileUse(
            TextureState& texture,
            size_t index,
            const std::optional<TexelBox>& box,
            QueueType queue,
            std::string_view list,
            uint64_t position,
            const Clock& ordered
        );

        /// Checks `use`, an indirect argument or count read of `buffer` on `queue`, against the last write of its bytes there and
        /// reports the rule of the entry that made it (VU-rojo-indirect-0005, 0011, 0017 or 0020).
        void replayIndirect(const TrackerLock& held, const BufferState& buffer, const UseEvent& use, QueueType queue, std::string_view list);

        /// Returns, per queue type, the position below which a remembered use belongs to no command still to be replayed on
        /// `queue` or later: the current position on `queue`, one past the last elsewhere. Segments merge only below it.
        std::array<uint64_t, 3> mergeFloor(QueueType queue) const;

        /// Replays one use at `position` on its queue: for a texture, the layout every subresource of its range has on the queue
        /// against the layouts that permit it, reporting VU-rojo-sync-0025 for a subresource handed over from another queue in
        /// a layout the two queues do not share, then VU-rojo-sync-0006, then the entry's own rule, then VU-rojo-sync-0004; a
        /// subresource whose latest layout event is on another queue that no fence orders before is left to VU-rojo-queue-0005.
        /// Then, for a sparse texture or buffer, the mapping changes of its tiles on other queues (VU-rojo-sparse-0013); then,
        /// for a texture or a buffer, its hazards on the queue (VU-rojo-sync-0021, then VU-rojo-sync-0024) and against every
        /// other queue (VU-rojo-queue-0005); then, for a sparse texture or buffer, the newly mapped tiles it reaches, which a
        /// texture's discard or a buffer's global barrier must order it after (VU-rojo-sparse-0014).
        void replayUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Judges the hazards of `use`, a use of a texture, against every subresource of `texture` it names, on its queue and
        /// against every other queue, and notes it.
        void replayTextureHazards(TextureState& texture, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Judges the hazards of `use`, a use of a buffer, against every segment of its range, on its queue and against every
        /// other queue, and notes it.
        void replayBufferUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Replays one global barrier recorded at `position`: every subresource of every texture and every segment of every
        /// buffer on `queue` gains its reach, and so does every newly mapped byte range it follows (VU-rojo-sparse-0014).
        void replayGlobal(const GlobalBarrier& barrier, QueueType queue, uint64_t position);

        /// Replays one buffer barrier recorded at `position`: exactly the segments of its range gain its reach.
        void replayBuffer(const TrackerLock& held, const BufferBarrier& barrier, QueueType queue, uint64_t position);

        /// Replays one texture barrier recorded at `position`: its old layout (VU-rojo-present-0014; for a subresource handed over
        /// from another queue, VU-rojo-sync-0025 for a barrier that does not discard and either starts elsewhere or takes over a
        /// layout the two queues do not share; then VU-rojo-sync-0013), then, for a transition, its source against the uses before it
        /// (VU-rojo-sync-0024) and, as a write of every tile, against the mapping changes of a sparse texture on other queues
        /// (VU-rojo-sparse-0013), the uses of every other queue and, for a placed texture, other resources' uses of its heap
        /// bytes there (VU-rojo-queue-0005), then, for a sparse texture, the newly mapped tiles of the subresource, which only a
        /// discard from `Undefined` may reach and which it clears (VU-rojo-sparse-0014), then its transition, the subresource's
        /// new layout event. A barrier that keeps the layout sets no layout event and gains its reach over exactly its
        /// subresources.
        void replayTexture(const TrackerLock& held, const TextureBarrier& barrier, QueueType queue, std::string_view list, uint64_t position);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, TextureState> m_textures; ///< The followed textures, by handle bits.
        std::unordered_map<uint64_t, BufferState> m_buffers;   ///< The followed buffers, by handle bits.
        std::unordered_map<uint64_t, HeapState> m_heaps;       ///< The heaps of followed placed resources, by handle bits.
        /// The position on each queue type of the last event there: each command replayed, each submission once its lists are
        /// replayed, and each `mapTiles` call takes the next one, so a use's position orders it against the barriers of the same
        /// queue and a clock that reaches a position covers everything before it on that queue.
        std::array<uint64_t, 3> m_positions = {};
        /// Per queue type, the join of the clocks of every fence value a submission there waited on: the work every later
        /// submission there is ordered after, its own component reached only through a fence.
        std::array<Clock, 3> m_waited = {};
        /// What the CPU has observed complete: `Device::wait` returning true, `completedValue` reporting a value, and
        /// `beginFrame` retiring a slot.
        Clock m_host = {};
        std::unordered_map<uint64_t, FenceClocks> m_fences; ///< The clocks of the values submitted for signal, by fence handle bits.
        std::map<uint64_t, Clock> m_frames;                 ///< The graphics clock at each ended frame's end, until its slot retires.
        /// The position on each queue type when the open frame began: a submission above it is the frame's.
        std::array<uint64_t, 3> m_frameStart = {};
        /// The position on each queue type of its last submission; zero before the first.
        std::array<uint64_t, 3> m_submitted = {};
    };

    /// Does the work of `lockTracker` in a Debug build.
    TrackerLock lockTrackerChecked(DeviceCore& core);

    /// Does the work of `replayLists` in a Debug build.
    void replayListsChecked(
        DeviceCore& core,
        QueueType type,
        std::span<const CommandList> lists,
        std::span<const FenceValue> wait,
        std::span<const FenceValue> signal,
        const TrackerLock& held
    );

    /// Does the work of `observeFence` in a Debug build.
    void observeFenceChecked(DeviceCore& core, Fence fence, uint64_t value);

    /// Does the work of `endFrameClock` in a Debug build.
    void endFrameClockChecked(DeviceCore& core, uint64_t frame);

    /// Does the work of `beginFrameClock` in a Debug build.
    void beginFrameClockChecked(DeviceCore& core, uint64_t retired);

    /// Does the work of `mapTileChanges` in a Debug build.
    void mapTileChangesChecked(DeviceCore& core, QueueType queue, std::span<const TileChange> changes);

    /// Does the work of `forgetFence` in a Debug build.
    void forgetFenceChecked(DeviceCore& core, Fence fence);

    /// Does the work of `checkPresentLayout` in a Debug build.
    void checkPresentLayoutChecked(DeviceCore& core, Texture image);

    /// Does the work of `trackTexture` in a Debug build.
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

    /// Does the work of `forgetTexture` in a Debug build.
    void forgetTextureChecked(DeviceCore& core, Texture texture);

    /// Does the work of `trackBuffer` in a Debug build.
    void trackBufferChecked(DeviceCore& core, Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse);

    /// Does the work of `forgetBuffer` in a Debug build.
    void forgetBufferChecked(DeviceCore& core, Buffer buffer);

    /// Does the work of `forgetHeap` in a Debug build.
    void forgetHeapChecked(DeviceCore& core, Heap heap);

    /// Locks the tracker of `core` when it validates; otherwise, and in a Release build, returns an empty lock. A backend takes
    /// it in `Queue::submit` before `checkSubmit` and keeps it until `recordSignals` returns.
    inline TrackerLock lockTracker([[maybe_unused]] DeviceCore& core)
    {
#ifndef NDEBUG
        return lockTrackerChecked(core);
#else
        return TrackerLock{};
#endif
    }

    /// Replays a submission to a queue of `type` under `held`, which `lockTracker` returned: orders it after the values of
    /// `wait`, replays the logs of `lists`, accepted by `submitLists`, in span order, and records the queue's running clock for
    /// each value of `signal`. A backend calls it in `Queue::submit` after `checkSubmit` and before `recordSignals`, so a value
    /// is never published without its clock. Inline, so a Release build compiles the call to nothing.
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

    /// Notes, under the tracker's lock, that the CPU observed `fence` of `core` reach `value` when `core` validates. The fence
    /// calls run it after reading the fence's record, never holding its pool lock. Inline, as `replayLists` is.
    inline void observeFence([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Fence fence, [[maybe_unused]] uint64_t value)
    {
#ifndef NDEBUG
        observeFenceChecked(core, fence, value);
#endif
    }

    /// Checks, under the tracker's lock, that the graphics queue's running clock covers the compute and copy submissions of
    /// frame `frame` of `core` (VU-rojo-device-0006), and records it as what the frame ordered when it ended, when `core`
    /// validates. Inline, as `replayLists` is.
    inline void endFrameClock([[maybe_unused]] DeviceCore& core, [[maybe_unused]] uint64_t frame)
    {
#ifndef NDEBUG
        endFrameClockChecked(core, frame);
#endif
    }

    /// Notes, under the tracker's lock, that `beginFrame` opened a frame of `core`, and joins what frame `retired`, zero for
    /// none, ordered into the host clock as it retires that frame's slot, when `core` validates. Inline, as `replayLists` is.
    inline void beginFrameClock([[maybe_unused]] DeviceCore& core, [[maybe_unused]] uint64_t retired)
    {
#ifndef NDEBUG
        beginFrameClockChecked(core, retired);
#endif
    }

    /// Checks, under the tracker's lock, the mapping changes `changes` of one `Queue::mapTiles` call on a queue of `queue` of
    /// `core` against the uses of their tiles on other queues, and notes them, when `core` validates
    /// (`Tracker::mapTiles`). The caller reads the records it builds them from before, never holding the tracker's lock.
    /// Inline, as `replayLists` is.
    inline void mapTileChanges([[maybe_unused]] DeviceCore& core, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::span<const TileChange> changes)
    {
#ifndef NDEBUG
        mapTileChangesChecked(core, queue, changes);
#endif
    }

    /// Forgets the clocks of a fence destroyed on `core`. Inline, as `replayLists` is.
    inline void forgetFence([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Fence fence)
    {
#ifndef NDEBUG
        forgetFenceChecked(core, fence);
#endif
    }

    /// Starts following a texture created or acquired on `core` when it validates, as `Tracker::addTexture` describes. Inline,
    /// as `replayLists` is.
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

    /// Checks, under the tracker's lock, that the swapchain image `image` presented on `core` is in `Layout::Present`
    /// (VU-rojo-present-0018) when `core` validates. Inline, as `replayLists` is.
    inline void checkPresentLayout([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Texture image)
    {
#ifndef NDEBUG
        checkPresentLayoutChecked(core, image);
#endif
    }

    /// Stops following a texture destroyed or presented on `core`. Inline, as `replayLists` is.
    inline void forgetTexture([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Texture texture)
    {
#ifndef NDEBUG
        forgetTextureChecked(core, texture);
#endif
    }

    /// Starts following a buffer created on `core` when it validates, as `Tracker::addBuffer` describes. A frame-arena page is
    /// not followed: the CPU writes frame data and the GPU only reads it within its frame. Inline, as `replayLists` is.
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

    /// Stops following a buffer destroyed on `core`. Inline, as `replayLists` is.
    inline void forgetBuffer([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Buffer buffer)
    {
#ifndef NDEBUG
        forgetBufferChecked(core, buffer);
#endif
    }

    /// Forgets the uses made of the bytes of a heap destroyed on `core`. Inline, as `replayLists` is.
    inline void forgetHeap([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Heap heap)
    {
#ifndef NDEBUG
        forgetHeapChecked(core, heap);
#endif
    }
} // namespace rojoRHI::core
