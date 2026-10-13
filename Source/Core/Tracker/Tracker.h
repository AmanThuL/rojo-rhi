//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.h
/// @brief Declares the validation tracker's state of a device, its mutex, the replay of submitted lists and the device hooks.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CommandList.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Layout.h>
#include <rojoRHI/QueueType.h>

#include "Core/Tracker/TrackerLog.h"

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
    /// texture. Lists record events without it; `Queue::submit` replays their logs into it in queue submission order. Every
    /// member takes or requires its mutex, so queues submitting on several threads see one order.
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

        /// Replays `log`, the log of the list labelled `list` submitted to a queue of `queue`, event by event in recording order,
        /// and terminates at the first violation. `held` must be a lock `lock` returned.
        void replay(const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list);

      private:
        /// The tracker's view of one texture.
        struct TextureState
        {
            std::string label;           ///< The texture's label, for reports.
            uint32_t mips = 1;           ///< Mip levels.
            uint32_t layers = 1;         ///< Array layers.
            std::vector<Layout> layouts; ///< One layout per subresource, at `mip * layers + layer`.
            bool acquired = false;       ///< A swapchain image no barrier has been replayed on since its acquire.
        };

        /// Replays one use.
        void replayUse(const UseEvent& use, QueueType queue, std::string_view list);

        /// Replays one global barrier.
        void replayGlobal(const GlobalBarrier& barrier, QueueType queue, std::string_view list);

        /// Replays one buffer barrier.
        void replayBuffer(const BufferBarrier& barrier, QueueType queue, std::string_view list);

        /// Replays one texture barrier: its old layout (VU-rojo-present-0014, then VU-rojo-sync-0013), then its transition.
        void replayTexture(const TextureBarrier& barrier, QueueType queue, std::string_view list);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, TextureState> m_textures; ///< The followed textures, by handle bits.
    };

    /// Does the work of `lockTracker` in a Debug build.
    TrackerLock lockTrackerChecked(DeviceCore& core);

    /// Does the work of `replayLists` in a Debug build.
    void replayListsChecked(DeviceCore& core, QueueType type, std::span<const CommandList> lists, const TrackerLock& held);

    /// Does the work of `trackTexture` in a Debug build.
    void trackTextureChecked(DeviceCore& core, Texture texture, std::string_view label, uint32_t mips, uint32_t layers, Layout initial, bool acquired);

    /// Does the work of `forgetTexture` in a Debug build.
    void forgetTextureChecked(DeviceCore& core, Texture texture);

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

    /// Stops following a texture destroyed or presented on `core`. Inline, as `replayLists` is.
    inline void forgetTexture([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Texture texture)
    {
#ifndef NDEBUG
        forgetTextureChecked(core, texture);
#endif
    }
} // namespace rojoRHI::core
