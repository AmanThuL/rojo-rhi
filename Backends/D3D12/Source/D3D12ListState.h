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
#include <optional>
#include <string>
#include <vector>

namespace rojoRHI {
    struct CommandListImpl;
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    struct BufferPayload;
    struct PipelineState;

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
        const PipelineState* pipeline = nullptr;         ///< The payload of the caller's bound pipeline; null for none.
    };

    /// The dynamic and input-assembler state the native list of a Graphics list holds, so a pass's starting values, the setters,
    /// `setPipeline` and the indexed draws send a native call only for a change. Reset at each acquisition: the state a native
    /// list keeps across its `Reset` is unknown. Render passes leave it as it is, so it carries from one pass to the next.
    struct DynamicState
    {
        std::optional<uint8_t> stencilReference;         ///< The stencil reference `OMSetStencilRef` last set; empty while unknown.
        std::optional<std::array<float, 4>> blendFactor; ///< The factors `OMSetBlendFactor` last set; empty while unknown.
        /// The recorded constant, slope-scaled and clamp bias; zero at each pass. While the caller's bound pipeline has
        /// `depthBias`, the native list holds these values.
        std::array<float, 3> depthBias = {};
        std::optional<D3D12_VIEWPORT> viewport; ///< The viewport `RSSetViewports` last set; empty while unknown.
        std::optional<D3D12_RECT> scissor;      ///< The scissor rectangle `RSSetScissorRects` last set; empty while unknown.
        /// The topology `IASetPrimitiveTopology` last set; undefined while unknown.
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        /// The index buffer `IASetIndexBuffer` last set; a zero location while unknown, which no buffer has.
        D3D12_INDEX_BUFFER_VIEW indexBuffer = {};
    };

    /// The native description of the render pass a list began last, the list's own, so recording allocates nothing and the
    /// resolve parameters `BeginRenderPass` points at stay where they are until `EndRenderPass`. Rewritten at each pass.
    struct PassStorage
    {
        /// The pass's render targets, the first `targetCount` used.
        std::array<D3D12_RENDER_PASS_RENDER_TARGET_DESC, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets = {};
        /// The resolve of each target that names a resolve target, the first `resolveCount` used, in target order.
        std::array<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> resolves = {};
        /// The barrier of each resolve target, moving it to `LAYOUT_RESOLVE_DEST` before the pass; `EndRenderPass` reverses it
        /// in place, so after the pass it holds the barrier back to `LAYOUT_RENDER_TARGET`.
        std::array<D3D12_TEXTURE_BARRIER, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> resolveBarriers = {};
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth = {}; ///< The depth-stencil attachment, when `hasDepth`.
        uint32_t targetCount = 0;                        ///< The render targets of the pass.
        uint32_t resolveCount = 0;                       ///< The resolve targets of the pass.
        bool hasDepth = false;                           ///< The pass has a depth-stencil attachment.
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
        DynamicState dynamic;                  ///< The dynamic state the native list holds.
        PassStorage pass;                      ///< The native description of the list's last render pass.
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

    /// Brings the compute side of `list` to what the caller bound before a dispatch, each call only where the native list
    /// holds something else: the global root signature after a backend kernel switched it, the bound pipeline after another
    /// displaced it, and the root block after `setRoot` or a root signature switch. A compute pipeline must be bound.
    void flushCompute(CommandListImpl& list);

    /// Brings the graphics side of `list` to what the caller bound before a draw, each call only where the native list holds
    /// something else: the bound pipeline, with its recorded depth bias when it has `depthBias`, after another displaced it,
    /// and the root block after `setRoot`. A list with no root binds none. It writes no draw-record constant: a direct draw
    /// writes it, and an indirect draw's command signature does. A graphics or mesh pipeline must be bound.
    void flushGraphics(CommandListImpl& list);

    /// Sets the view of the whole of `buffer`, from offset 0, as indices of `type` on `list`, unless the native list holds
    /// that view: the indices of `type` that fit in the buffer and in the view's 32-bit size. Every indexed draw, direct or
    /// indirect, binds its buffer here, so draws of one buffer share the view.
    void applyIndexBuffer(CommandListImpl& list, const BufferPayload& buffer, IndexType type);
} // namespace rojoRHI::d3d12
