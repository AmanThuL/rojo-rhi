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
    struct SubmitTimingState;

    enum class ComputeRoot : uint8_t
    {
        None, // The first dispatch of a caller pipeline sets the global one.
        Global,
        Kernel,
        Foreign // Vendor work recorded on the list, such as an upscale.
    };

    // Kept lazily: `setRoot` stores the address and marks both sides dirty, and the next draw or dispatch binds it on
    // its side. Reset at each acquisition.
    struct Binding
    {
        D3D12_GPU_VIRTUAL_ADDRESS root = 0;
        bool graphicsRootDirty = false;
        bool computeRootDirty = false;
        bool drawIdDirty = true;
        bool graphicsSignatureDirty = false; // Vendor work may have set a root signature of its own.
        ComputeRoot computeRoot = ComputeRoot::None;
        ID3D12PipelineState* computePipeline = nullptr;
        ID3D12PipelineState* graphicsPipeline = nullptr;
        const PipelineState* pipeline = nullptr;
    };

    // What the native list of a Graphics list holds, so a pass's starting values, the setters, `setPipeline` and the
    // indexed draws send a native call only for a change; an empty optional is unknown. Reset at each acquisition,
    // since a native list's state across `Reset` is unknown; render passes leave it, so it carries from one pass to the
    // next.
    struct DynamicState
    {
        std::optional<uint8_t> stencilReference;
        std::optional<std::array<float, 4>> blendFactor;
        // The open pass's starting stencil reference, 0, is owed: the native list holds another or an unknown value and
        // no draw has read it since the pass began. The first draw whose pipeline reads it sends it;
        // `setStencilReference` settles it.
        bool stencilReferenceOwed = false;
        bool blendFactorOwed = false; // As `stencilReferenceOwed`, for the starting blend factor of zeros.
        // Constant, slope-scaled and clamp; zero at each pass. The native list holds them while the bound pipeline has
        // `depthBias`.
        std::array<float, 3> depthBias = {};
        std::optional<D3D12_VIEWPORT> viewport;
        std::optional<D3D12_RECT> scissor;
        // Undefined while unknown.
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        // A zero location while unknown, which no buffer has.
        D3D12_INDEX_BUFFER_VIEW indexBuffer = {};
        Buffer indexHandle; // Null while unknown.
        IndexType indexType = IndexType::UInt32;
    };

    // So a root pushed with `pushRoot` binds without a lookup in the heap directory. Reset at each acquisition.
    struct LastUpload
    {
        Buffer page;
        const BufferPayload* payload = nullptr; // Fixed while the page lives.
        Addr address = {};
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    };

    // Each by the view-heap index of its raw SRV.
    struct IndirectBuffers
    {
        uint32_t recordsIndex = 0; // 0, the reserved slot, while unknown.
        const BufferPayload* records = nullptr;
        uint32_t countIndex = 0; // 0 also for a null count.
        const BufferPayload* count = nullptr;
    };

    // The list's own, so recording allocates nothing and the resolve parameters `BeginRenderPass` points at stay where
    // they are until `EndRenderPass`. Rewritten at each pass.
    struct PassStorage
    {
        // The first `targetCount` used.
        std::array<D3D12_RENDER_PASS_RENDER_TARGET_DESC, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> targets = {};
        // The first `resolveCount` used, in target order.
        std::array<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> resolves = {};
        // Moves each resolve target to LAYOUT_RESOLVE_DEST before the pass; `EndRenderPass` reverses it in place, so
        // after the pass it holds the barrier back to LAYOUT_RENDER_TARGET.
        std::array<D3D12_TEXTURE_BARRIER, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> resolveBarriers = {};
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth = {};
        uint32_t targetCount = 0;
        uint32_t resolveCount = 0;
        bool hasDepth = false;
        // The caller's attachments the description was made from, so a pass that repeats them reuses it.
        std::array<ColorAttachment, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors = {};
        DepthAttachment depthAttachment = {};
        // False after acquisition, and after a pass with a resolve, whose barriers `EndRenderPass` reverses in place.
        bool reusable = false;
    };

    // Created when its frame slot's pool grows and kept while the pool hands the list out again. Everything recording
    // touches is the list's own, so recording takes no lock.
    struct ListState
    {
        Com<ID3D12CommandAllocator> allocator; // Reset when its frame slot is reused.
        Com<ID3D12GraphicsCommandList9> list;  // Reset at each acquisition, closed by `CommandList::end`.
        QueueType type = QueueType::Graphics;
        uint32_t frameSlot = 0;
        uint32_t ordinal = 0;
        std::string label;
        std::wstring scratch;
        // Kept across recordings: a scope with the label of the one before it widens nothing.
        std::wstring eventLabel;
        std::string eventSource;
        LastUpload upload;
        // So a repeated command finds them without the heap directory. Reset at each acquisition.
        IndirectBuffers indirect;
        Binding binding;
        BarrierScratch barriers;
        ListTimestamps timestamps;
        DynamicState dynamic;
        PassStorage pass;
        // Receives the recording's first and last timestamps; null for none.
        SubmitTimingState* submitTiming = nullptr;
        uint64_t submitPair = 0;
    };

    // Those taken since the slot was reused come first; the rest wait to be reused.
    struct ListPool
    {
        std::mutex mutex; // Acquisition is free-threaded.
        std::vector<std::unique_ptr<CommandListImpl>> lists;
        size_t used = 0;
    };

    // A device's list pools, one per frame slot and `QueueType`; a device uses its first `framesInFlight` slots.
    class ListPools
    {
      public:
        // A list the pool grows by is one explicit allocation, counted in `cpuAllocations`, with its own allocator and
        // a closed native list, both named `rojorhi.list.<slot>.<graphics|compute|copy>.<ordinal>` and counted as one
        // `nativeCreateCalls` each; a refused creation logs and aborts, in every build. The pool's mutex is a leaf
        // lock: acquisition is not recording.
        CommandListImpl& acquire(DeviceImpl& impl, uint32_t slot, QueueType type);

        // Once the slot's frame has retired. Resets each list's allocator, one `nativeHostCalls` per list; a refused
        // reset logs and aborts, in every build.
        void reset(DeviceImpl& impl, uint32_t slot);

        // Takes the pool's mutex, so it serves the retire hooks, never recording.
        CommandListImpl& at(uint32_t slot, QueueType type, uint32_t ordinal);

      private:
        std::array<std::array<ListPool, 3>, 3> m_slots; // Indexed by frame slot, then by `QueueType`.
    };

    // Read from the list's core without a lock.
    DeviceImpl& deviceOf(const CommandListImpl& list);

    // Each native call only where the native list holds something else: the global root signature at the first dispatch
    // or after a backend kernel or vendor work switched it, the bound pipeline after another displaced it, and the root
    // block after `setRoot` or a root signature switch. A compute pipeline must be bound.
    void flushCompute(CommandListImpl& list);

    // Each native call only where the native list holds something else: the global root signature after vendor work,
    // the bound pipeline, with its recorded depth bias when it has `depthBias`, after another displaced it, and the
    // root block after `setRoot` or a root signature switch. It writes no draw-record constant: a direct draw writes
    // it, and an indirect draw's command signature does. A graphics or mesh pipeline must be bound.
    void flushGraphics(CommandListImpl& list);

    // Vendor work may have set its own descriptor heaps, root signatures and pipeline. The device's heaps are set again
    // at once, one native call, because clears and fills read them as well as draws and dispatches. The rest is marked
    // unknown, so a root signature set later follows the heaps: the global root signature of each side, both root
    // blocks, the draw-record constant b1, the pipeline slot and the topology.
    void resumeAfterVendorWork(CommandListImpl& list);

    // Unless the native list holds that view. Every indexed draw, direct or indirect, binds its buffer here, so draws
    // of one buffer share the view and a draw of the buffer and type the list holds reads no payload.
    void applyIndexBuffer(CommandListImpl& list, Buffer indices, IndexType type);
} // namespace rojoRHI::d3d12
