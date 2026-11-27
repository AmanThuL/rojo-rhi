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

    // A handle's bits, slot, generation and tag, name one allocation for the pool's whole life: releasing a slot advances
    // its generation, and a slot whose generation reaches the limit is never reused. A payload is fixed from creation to
    // retirement. So a live handle names the payload cached for its bits, and an entry whose pipeline was destroyed
    // matches only that dead handle, never a live one. Kept across recordings, so a pooled list's first bind of a
    // pipeline it bound before reads no table. A hit reads no table either, so binding a destroyed pipeline, which
    // VU-rojo-cmd-0020 refuses in Debug and Release leaves unchecked, can read a retired payload, as a
    // list still recording after its frame retired (VU-rojo-device-0005) can.
    struct ResolvedPipelines
    {
        std::array<uint64_t, 2> handles = {}; // Newest first; 0 for an empty entry.
        std::array<const PipelineState*, 2> states = {};
    };

    // The reasons a draw has graphics state to send, one bit each of `Binding::graphicsDirty`, so a draw with nothing
    // to send tests one word. Each bit is set where its reason becomes true and cleared by the draw that settles it.
    // Another pipeline took the slot both sides share: a compute bind or dispatch, the fill kernel or vendor work.
    inline constexpr uint32_t kGraphicsPipelineDirty = 1u << 0;
    inline constexpr uint32_t kGraphicsRootDirty = 1u << 1;
    // The pass's starting stencil reference is owed and the bound pipeline reads it; `DynamicState` keeps what is owed
    // while a pipeline that reads nothing is bound, so its draws test no bit.
    inline constexpr uint32_t kStencilReferenceDirty = 1u << 2;
    inline constexpr uint32_t kBlendFactorDirty = 1u << 3; // As `kStencilReferenceDirty`, for the starting blend factor.
    // The draw-record constant b1 holds no value since the root signature was set, or an indirect draw left it: the runtime
    // resets it to 0 after ExecuteIndirect, so that write is redundant but kept. Only a direct draw writes 0 there.
    inline constexpr uint32_t kDrawIdDirty = 1u << 4;
    inline constexpr uint32_t kDirectDrawDirty = kGraphicsPipelineDirty | kGraphicsRootDirty | kStencilReferenceDirty | kBlendFactorDirty | kDrawIdDirty;
    // An indirect draw's command signature writes b1 itself, so it leaves that bit set for the next direct draw.
    inline constexpr uint32_t kIndirectDrawDirty = kDirectDrawDirty & ~kDrawIdDirty;

    // Kept lazily: `setRoot` stores the address and marks both sides dirty, and the next draw or dispatch binds it on
    // its side. Reset at each acquisition.
    struct Binding
    {
        D3D12_GPU_VIRTUAL_ADDRESS root = 0;
        uint32_t graphicsDirty = kDrawIdDirty;
        bool computeRootDirty = false;
        // The native list holds no global graphics root signature: a Graphics list from acquisition, any list after
        // vendor work. The next graphics `setPipeline` sets it, so a list that binds no graphics pipeline sends none.
        bool graphicsSignatureOwed = false;
        // The device's shader-visible heaps are set on the native list. The first command that reads them sets them, so
        // a list that only copies, uploads or fills sends no `SetDescriptorHeaps`.
        bool heapsSet = false;
        ComputeRoot computeRoot = ComputeRoot::None;
        ID3D12PipelineState* computePipeline = nullptr;
        ID3D12PipelineState* graphicsPipeline = nullptr;
        const PipelineState* pipeline = nullptr;

        bool dirty(uint32_t reasons) const
        {
            return (graphicsDirty & reasons) != 0;
        }
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
        // no draw has read it since the pass began. A pass begins with no pipeline bound, so `setPipeline` marks
        // `kStencilReferenceDirty` for a pipeline that reads it, and that pipeline's first draw sends it;
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

    // The arena page of the list's last upload, read from its payload once per page, so an upload writes nothing here
    // and a root anywhere in that page, as `pushRoot` binds, resolves without a lookup in the heap directory: an
    // address whose `lo` is the page's raw-view index names that page, which lives until the device is destroyed. Reset
    // at each acquisition.
    struct LastUpload
    {
        Buffer page;                        // Null until the list's first upload.
        uint32_t viewIndex = 0;             // The page's raw-view index, the `lo` of each of its addresses; 0 while none.
        D3D12_GPU_VIRTUAL_ADDRESS base = 0; // The page's GPU base address; 0 while none.
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
    // they are until `EndRenderPass`. Rewritten at each pass that does not repeat the description it holds.
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
        // The caller's attachments the description was made from, so a pass that repeats them reuses it, in a later
        // recording of the list too: a handle's bits name one texture for the pool's whole life and its payload is fixed
        // while it lives, so equal attachments of live textures describe the same native pass.
        std::array<ColorAttachment, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors = {};
        DepthAttachment depthAttachment = {};
        // False until the list's first pass, and after a pass with a resolve, whose barriers `EndRenderPass` reverses in
        // place.
        bool reusable = false;
    };

    // Created when its frame slot's pool grows and kept while the pool hands the list out again. Everything recording
    // touches is the list's own, so recording takes no lock. The fields acquisition writes or reads come first, so a
    // recording starts on few cache lines.
    struct ListState
    {
        Com<ID3D12CommandAllocator> allocator; // Reset when its frame slot is reused.
        Com<ID3D12GraphicsCommandList9> list;  // Reset at each acquisition, closed by `CommandList::end`.
        // Receives the recording's first and last timestamps; null for none.
        SubmitTimingState* submitTiming = nullptr;
        uint64_t submitPair = 0;
        QueueType type = QueueType::Graphics;
        uint32_t frameSlot = 0;
        uint32_t ordinal = 0;
        Binding binding;
        // The payloads of the last two pipelines `setPipeline` bound, so a list that alternates two reads no table. Kept
        // across recordings, unlike `Binding`.
        ResolvedPipelines resolved;
        LastUpload upload;
        // So a repeated command finds them without the heap directory. Reset at each acquisition.
        IndirectBuffers indirect;
        DynamicState dynamic;
        ListTimestamps timestamps;
        std::string label;
        std::wstring scratch;
        // Kept across recordings: a scope with the label of the one before it widens nothing.
        std::wstring eventLabel;
        std::string eventSource;
        // Kept across recordings, as the pass description is: a batch of one record that repeats the list's last one,
        // in an earlier recording too, sends the lowering again.
        BarrierScratch barriers;
        PassStorage pass;
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

    // Each native call only where the native list holds something else: the bound pipeline, with its recorded depth
    // bias when it has `depthBias`, after another displaced it, the root block after `setRoot` or a root signature
    // switch, the pass's owed starting values the pipeline reads, and, where `reasons` holds `kDrawIdDirty`, 0 to the
    // draw-record constant. `reasons` is `kDirectDrawDirty` or `kIndirectDrawDirty`; a draw with none of them set
    // returns after one test. A graphics or mesh pipeline must be bound, and the global root signature is the graphics
    // `setPipeline`'s to set.
    void flushGraphics(CommandListImpl& list, uint32_t reasons);

    // Vendor work may have set its own descriptor heaps, root signatures and pipeline. The device's heaps are set again
    // at once, one native call, so every later reader finds them as `Binding::heapsSet` says. The rest is marked
    // unknown, so a root signature set later follows the heaps: the global root signature of each side, owed to the
    // next graphics `setPipeline` and the next dispatch, both root blocks, the draw-record constant b1, the pipeline
    // slot and the topology.
    void resumeAfterVendorWork(CommandListImpl& list);

    // Sets the device's view and sampler heaps on the native list, one native call. Shaders index both directly, so
    // D3D12 needs them set before a root signature that indexes them is (HLSL SM 6.6 dynamic resources): a first
    // global root signature of either side, and a storage clear, call this first unless `Binding::heapsSet` says so.
    void setHeaps(CommandListImpl& list);

    // Unless the native list holds that view. Every indexed draw, direct or indirect, binds its buffer here, so draws
    // of one buffer share the view and a draw of the buffer and type the list holds reads no payload.
    void applyIndexBuffer(CommandListImpl& list, Buffer indices, IndexType type);
} // namespace rojoRHI::d3d12
