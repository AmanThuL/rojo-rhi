//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.h
/// @brief Declares the validation tracker's state of a device, its mutex, the replay of submitted lists and the device hooks.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CommandList.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Layout.h>
#include <rojoRHI/QueueType.h>

#include "Core/Tracker/ByteSegments.h"
#include "Core/Tracker/Hazards.h"
#include "Core/Tracker/TrackerLog.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

    /// The state the validation tracker of one device judges submitted work against: one layout per subresource of every live
    /// texture, and the hazard state per queue of every texture subresource and of the byte segments of every live buffer.
    /// Lists record events without it; `Queue::submit` replays their logs into it in queue submission order. Every member
    /// takes or requires its mutex, so queues submitting on several threads see one order.
    class Tracker
    {
      public:
        /// Locks the state. `Queue::submit` holds the lock from its wait check through replay to publishing its signals.
        TrackerLock lock();

        /// Starts following `texture`, labelled `label`, of `mips` mips and `layers` layers, with every subresource in
        /// `initial`. `acquired` marks a swapchain image whose first barrier must start from `Undefined` (VU-rojo-present-0014).
        /// Takes the lock.
        void addTexture(Texture texture, std::string_view label, uint32_t mips, uint32_t layers, Layout initial, bool acquired);

        /// Stops following `texture`, destroyed or presented; a texture the tracker does not follow is ignored. Takes the lock.
        void removeTexture(Texture texture);

        /// Returns the layout of mip `mip` and layer `layer` of `texture`, or nothing when the tracker does not follow it. Takes
        /// the lock.
        std::optional<Layout> layoutOf(Texture texture, uint32_t mip, uint32_t layer);

        /// Starts following `buffer`, labelled `label`, of `size` bytes, with no hazard on any queue. A placed buffer names its
        /// `heap` and the byte offset `heapOffset` it occupies there; a dedicated one passes a null heap. Takes the lock.
        void addBuffer(Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset);

        /// Stops following `buffer`, destroyed; a buffer the tracker does not follow is ignored. Takes the lock.
        void removeBuffer(Buffer buffer);

        /// Returns the number of byte segments the tracker keeps for `buffer`, or nothing when it does not follow it. Takes the
        /// lock.
        std::optional<size_t> segmentCount(Buffer buffer);

        /// Returns how the last write on `queue` to `size` bytes from `offset` of `buffer` is ordered before a use of `use`: for
        /// the bytes of several segments, the worst of them, `Missing` before `Unordered` before `Ordered` before `None`. A
        /// buffer the tracker does not follow, such as a frame-arena page, has no write. A check that reports its own rule ahead
        /// of VU-rojo-sync-0021 and VU-rojo-sync-0024 calls it during replay. `held` must be a lock `lock` returned.
        WriteOrder lastWriteOrder(const TrackerLock& held, QueueType queue, Buffer buffer, uint64_t offset, uint64_t size, Scope use);

        /// Replays `log`, the log of the list labelled `list` submitted to a queue of `queue`, event by event in recording order,
        /// and terminates at the first violation. `held` must be a lock `lock` returned.
        void replay(const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list);

        /// Checks that every subresource of `image`, a swapchain image presented to the queue, is in `Layout::Present`
        /// (VU-rojo-present-0018); an image the tracker does not follow is ignored. `held` must be a lock `lock` returned.
        void checkPresent(const TrackerLock& held, Texture image);

      private:
        /// The tracker's view of one texture.
        struct TextureState
        {
            std::string label;                ///< The texture's label, for reports.
            uint32_t mips = 1;                ///< Mip levels.
            uint32_t layers = 1;              ///< Array layers.
            std::vector<Layout> layouts;      ///< One layout per subresource, at `mip * layers + layer`.
            std::vector<UnitHazards> hazards; ///< The hazard state of each subresource, indexed as `layouts`.
            bool acquired = false;            ///< A swapchain image no barrier has been replayed on since its acquire.
        };

        /// The tracker's view of one buffer.
        struct BufferState
        {
            std::string label;       ///< The buffer's label, for reports.
            uint64_t size = 0;       ///< Bytes.
            Heap heap = {};          ///< The heap the buffer is placed in; null for a dedicated allocation.
            uint64_t heapOffset = 0; ///< The byte offset of the buffer in `heap`; its heap range runs `size` bytes from there.
            ByteSegments segments;   ///< The hazard state of its bytes.
        };

        /// Returns the state of `texture`, or null when the tracker does not follow it. Unlike the public lookups it takes no
        /// lock, so the replay, which holds one, may call it.
        TextureState* locate(const TrackerLock& held, Texture texture);

        /// Returns the state of `buffer`, or null when the tracker does not follow it, as the texture lookup does.
        BufferState* locate(const TrackerLock& held, Buffer buffer);

        /// Returns, per queue type, the position below which a remembered use belongs to no command still to be replayed on
        /// `queue` or later: the current position on `queue`, one past the last elsewhere. Segments merge only below it.
        std::array<uint64_t, 3> mergeFloor(QueueType queue) const;

        /// Replays one use at `position` on its queue: for a texture, the layout of every subresource of its range against the
        /// layouts that permit it, reporting VU-rojo-sync-0006, then the entry's own rule, then VU-rojo-sync-0004; then, for a
        /// texture or a buffer, its hazards (VU-rojo-sync-0021, then VU-rojo-sync-0024).
        void replayUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Judges the hazards of `use`, a use of a texture, against every subresource of `texture` it names, and notes it.
        void replayTextureHazards(TextureState& texture, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Judges the hazards of `use`, a use of a buffer, against every segment of its range, and notes it.
        void replayBufferUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position);

        /// Replays one global barrier recorded at `position`: every subresource of every texture and every segment of every
        /// buffer on `queue` gains its reach.
        void replayGlobal(const GlobalBarrier& barrier, QueueType queue, uint64_t position);

        /// Replays one buffer barrier recorded at `position`: exactly the segments of its range gain its reach.
        void replayBuffer(const TrackerLock& held, const BufferBarrier& barrier, QueueType queue, uint64_t position);

        /// Replays one texture barrier recorded at `position`: its old layout (VU-rojo-present-0014, then VU-rojo-sync-0013),
        /// then, for a transition, its source against the uses before it (VU-rojo-sync-0024), then its transition. A barrier that
        /// keeps the layout gains its reach over exactly its subresources.
        void replayTexture(const TrackerLock& held, const TextureBarrier& barrier, QueueType queue, std::string_view list, uint64_t position);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, TextureState> m_textures; ///< The followed textures, by handle bits.
        std::unordered_map<uint64_t, BufferState> m_buffers;   ///< The followed buffers, by handle bits.
        /// The position on each queue type of the last command replayed there: each command takes the next one, so a use's
        /// position orders it against the barriers of the same queue.
        std::array<uint64_t, 3> m_positions = {};
    };

    /// Does the work of `lockTracker` in a Debug build.
    TrackerLock lockTrackerChecked(DeviceCore& core);

    /// Does the work of `replayLists` in a Debug build.
    void replayListsChecked(DeviceCore& core, QueueType type, std::span<const CommandList> lists, const TrackerLock& held);

    /// Does the work of `checkPresentLayout` in a Debug build.
    void checkPresentLayoutChecked(DeviceCore& core, Texture image);

    /// Does the work of `trackTexture` in a Debug build.
    void trackTextureChecked(DeviceCore& core, Texture texture, std::string_view label, uint32_t mips, uint32_t layers, Layout initial, bool acquired);

    /// Does the work of `forgetTexture` in a Debug build.
    void forgetTextureChecked(DeviceCore& core, Texture texture);

    /// Does the work of `trackBuffer` in a Debug build.
    void trackBufferChecked(DeviceCore& core, Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset);

    /// Does the work of `forgetBuffer` in a Debug build.
    void forgetBufferChecked(DeviceCore& core, Buffer buffer);

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

    /// Replays the logs of `lists`, submitted to a queue of `type` and accepted by `submitLists`, in span order, under `held`,
    /// which `lockTracker` returned. A backend calls it in `Queue::submit` after `checkSubmit`. Inline, so a Release build
    /// compiles the call to nothing.
    inline void replayLists(
        [[maybe_unused]] DeviceCore& core,
        [[maybe_unused]] QueueType type,
        [[maybe_unused]] std::span<const CommandList> lists,
        [[maybe_unused]] const TrackerLock& held
    )
    {
#ifndef NDEBUG
        replayListsChecked(core, type, lists, held);
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
        [[maybe_unused]] bool acquired
    )
    {
#ifndef NDEBUG
        trackTextureChecked(core, texture, label, mips, layers, initial, acquired);
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
        [[maybe_unused]] uint64_t heapOffset
    )
    {
#ifndef NDEBUG
        trackBufferChecked(core, buffer, label, size, heap, heapOffset);
#endif
    }

    /// Stops following a buffer destroyed on `core`. Inline, as `replayLists` is.
    inline void forgetBuffer([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Buffer buffer)
    {
#ifndef NDEBUG
        forgetBufferChecked(core, buffer);
#endif
    }
} // namespace rojoRHI::core
