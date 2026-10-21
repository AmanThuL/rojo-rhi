//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ListState.h
/// @brief Declares the native state a D3D12 command list owns and records into, and the list pools of each frame slot.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Barriers.h"
#include "D3D12Common.h"
#include "D3D12Timestamps.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rojoRHI {
    struct CommandListImpl;
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    /// The root signature the compute side of a list holds: the global one of caller pipelines, or the backend kernels' one.
    enum class ComputeRoot : uint8_t
    {
        Global, ///< The global root signature, set at acquisition on Graphics and Compute lists.
        Kernel  ///< The kernel root signature, set by a backend kernel.
    };

    /// The root binding of a list, kept lazily: `setRoot` stores the address and marks both sides dirty, and the next draw or
    /// dispatch binds it on its side. Reset at each acquisition.
    struct Binding
    {
        D3D12_GPU_VIRTUAL_ADDRESS root = 0;              ///< The root block's GPU virtual address; 0 until `setRoot`.
        bool graphicsRootDirty = false;                  ///< The graphics side has not bound `root` yet.
        bool computeRootDirty = false;                   ///< The compute side has not bound `root` yet.
        bool drawIdDirty = true;                         ///< The draw-record constant has not been set since acquisition.
        ComputeRoot computeRoot = ComputeRoot::Global;   ///< The root signature the compute side holds.
        ID3D12PipelineState* computePipeline = nullptr;  ///< The pipeline the compute side holds; null for none.
        ID3D12PipelineState* graphicsPipeline = nullptr; ///< The pipeline the graphics side holds; null for none.
    };

    /// The native objects and recording state of one command list, created when its frame slot's pool grows and kept while
    /// the pool hands the list out again. Everything recording touches is the list's own, so recording takes no lock.
    struct ListState
    {
        Com<ID3D12CommandAllocator> allocator; ///< The list's own allocator, reset when its frame slot is reused.
        Com<ID3D12GraphicsCommandList9> list;  ///< Reset at each acquisition, closed by `CommandList::end`.
        QueueType type = QueueType::Graphics;  ///< The queue type of the pool the list belongs to.
        uint32_t frameSlot = 0;                ///< The frame slot of the pool the list belongs to.
        uint32_t ordinal = 0;                  ///< The list's index in its pool.
        std::string label;                     ///< The label the native list carries.
        std::wstring scratch;                  ///< UTF-16 scratch for the list's label and its scope labels.
        Binding binding;                       ///< The lazy root binding.
        BarrierScratch barriers;               ///< The scratch barrier batches are lowered into.
        ListTimestamps timestamps;             ///< The timestamps the list records.
    };

    /// The command lists of one frame slot: those taken since the slot was reused come first and the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex;                                    ///< Guards the pool, because acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists; ///< Every list the slot ever needed.
        size_t used = 0;                                     ///< How many of `lists` the open frame has taken.
    };

    /// The command list pools of a device, one per frame slot and `QueueType`; a device uses its first `framesInFlight` slots.
    class ListPools
    {
      public:
        /// Takes a list of frame slot `slot`'s pool of `type` on `impl`, reusing one when the pool has one left, and returns it.
        /// A list the pool grows by is one explicit allocation, counted in `cpuAllocations`, and gets its own allocator and a
        /// closed native list of `type`, both named `rojorhi.list.<slot>.<graphics|compute|copy>.<ordinal>` and counted as one
        /// `nativeCreateCalls` each; a refused creation logs and aborts, in every build. The pool's mutex is a leaf lock:
        /// acquisition is not recording.
        CommandListImpl& acquire(DeviceImpl& impl, uint32_t slot, QueueType type);

        /// Returns every list frame slot `slot` took to its pools, once the slot's frame has retired, after resetting the
        /// allocator of each, one `nativeHostCalls` per list; a refused reset logs and aborts, in every build.
        void reset(DeviceImpl& impl, uint32_t slot);

        /// Returns list `ordinal` of frame slot `slot`'s pool of `type`, which the pool has made. It takes the pool's mutex, so
        /// it serves the retire hooks, never recording.
        CommandListImpl& at(uint32_t slot, QueueType type, uint32_t ordinal);

      private:
        std::array<std::array<ListPool, 3>, 3> m_slots; ///< Indexed by frame slot, then by `QueueType`.
    };

    /// Returns the device that acquired `list`, read from the list's core without a lock.
    DeviceImpl& deviceOf(const CommandListImpl& list);
} // namespace rojoRHI::d3d12
