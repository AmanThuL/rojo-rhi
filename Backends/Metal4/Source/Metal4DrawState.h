//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DrawState.h
/// @brief Declares what a Metal 4 command list keeps of its open render pass, and the lazy opening, splitting and ending of its render encoder.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/ScopeRecord.h"
#include "Metal4Icb.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI {
    struct CommandListImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    struct PipelineState;

    struct DepthBias
    {
        float constant = 0.0f;    // In units of the format's minimum resolvable depth difference.
        float slopeScaled = 0.0f; // Times the polygon's maximum depth slope.
        float clamp = 0.0f;       // Zero for none.

        bool operator==(const DepthBias&) const = default;
    };

    // As the list last set it; a new encoder holds Metal's defaults, which are these starting values. A setter sends
    // Metal only a value that differs.
    struct RenderHeld
    {
        const PipelineState* pipeline = nullptr;
        const MTL::DepthStencilState* depthStencil = nullptr; // Null is Metal's default.
        MTL::CullMode cull = MTL::CullModeNone;
        MTL::Winding winding = MTL::WindingClockwise;
        MTL::TriangleFillMode fill = MTL::TriangleFillModeFill;
        MTL::DepthClipMode depthClip = MTL::DepthClipModeClip;
        DepthBias bias;
        uint32_t stencilReference = 0;
        std::array<float, 4> blendColor = {};
        bool viewportKnown = false; // Metal's default viewport is unknown here.
        MTL::Viewport viewport = {};
        bool scissorKnown = false;
        MTL::ScissorRect scissor = {};
        // A new encoder's first draw binds the table to the vertex and fragment stages only; the first mesh pipeline set on
        // it adds the object and mesh stages.
        bool meshStages = false;
    };

    // A handle's bits name one allocation for the pool's whole life and a payload is fixed from creation to retirement,
    // so a handle live when bound names the payload cached for its bits. A hit reads no table, so a list still
    // recording after its frame retired, which breaks VU-rojo-device-0005 and goes unchecked in Release, can read a
    // retired payload. Emptied at each acquisition.
    struct ResolvedPipelines
    {
        std::array<uint64_t, 2> handles = {}; // Newest first; 0 for an empty entry.
        std::array<const PipelineState*, 2> states = {};
    };

    // Recorded while the pass's render encoder is closed.
    enum class PrefixKind : uint8_t
    {
        Pipeline,
        Viewport,
        Scissor,
        StencilReference,
        BlendColor,
        DepthBias,
        Execute,         // A counted draw's indirect command buffer, run over the range its kernel wrote.
        DrawIndirect,    // A counted draw of one record: its record bound at `buffer(3)`, one native indirect draw.
        IndexedIndirect, // A counted indexed draw of one record: its record bound at `buffer(3)`, one native indexed draw.
        MeshExecute,     // A counted mesh draw's indirect command buffer, run over the range its kernel wrote.
        MeshIndirect,    // A counted mesh draw of one record: its record bound at `buffer(3)`, one native indirect mesh draw.
    };

    inline constexpr size_t kPrefixKinds = 11;

    // The leading `uint32` a vertex, task or mesh entry reads as `drawId`.
    inline constexpr uint64_t kZeroRecordBytes = 4;

    struct PrefixExecute
    {
        const MTL::IndirectCommandBuffer* buffer;
        uint64_t range; // The GPU address of its `MTL::IndirectCommandBufferExecutionRange`.
    };

    // The draw binds the caller's record at `buffer(3)` and the root recorded before it at `buffer(0)`; the primitive
    // type is the one in force when the draw was recorded.
    struct PrefixIndirect
    {
        uint64_t drawRecord; // The GPU address of the caller's `DrawIndirectArgs` or `DrawIndexedIndirectArgs` record.
        uint64_t record;     // The GPU address of the `MTL::Draw*IndirectArguments` record.
        MTL::PrimitiveType primitive;
        MTL::IndexType indexType; // IndexedIndirect only, as are indices and indexLength.
        uint64_t indices;
        uint64_t indexLength; // In bytes.
        uint64_t root;        // The root recorded before the draw; 0 for none.
    };

    // The draw binds the caller's record at `buffer(3)` and the root recorded before it at `buffer(0)`, and takes the
    // threadgroup sizes of the pipeline bound when it was recorded.
    struct PrefixMesh
    {
        uint64_t record; // The GPU address of the `DrawMeshTasksIndirectArgs` record.
        uint64_t native; // The GPU address of the `MTL::DispatchThreadgroupsIndirectArguments` record.
        const PipelineState* pipeline;
        uint64_t root; // The root recorded before the draw; 0 for none.
    };

    struct PrefixCommand
    {
        PrefixKind kind = PrefixKind::Pipeline; // Which member of the union holds the value.
        union {
            const PipelineState* pipeline;
            MTL::Viewport viewport;
            MTL::ScissorRect scissor;
            uint32_t stencilReference;
            std::array<float, 4> color;
            std::array<float, 3> bias; // Constant, slope-scaled and clamp.
            PrefixExecute execute;
            PrefixIndirect indirect;
            PrefixMesh mesh;
        };
    };

    // Copied at `beginRenderPass` with its native textures.
    struct PassColor
    {
        const MTL::Texture* texture = nullptr;
        uint32_t mip = 0;
        uint32_t layer = 0; // A cube face is a layer.
        LoadOp load = LoadOp::Load;
        StoreOp store = StoreOp::Store;
        MTL::ClearColor clear{ 0.0, 0.0, 0.0, 0.0 }; // Used when `load` is `Clear`.
        const MTL::Texture* resolve = nullptr;
    };

    // Copied at `beginRenderPass`; `texture` is null when the pass has no depth attachment.
    struct PassDepth
    {
        const MTL::Texture* texture = nullptr;
        uint32_t mip = 0;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Load;
        StoreOp store = StoreOp::Store;
        double clearDepth = 0.0;
        bool stencil = false; // The format has a stencil plane, which the stencil fields govern.
        LoadOp stencilLoad = LoadOp::DontCare;
        StoreOp stencilStore = StoreOp::DontCare;
        uint32_t clearStencil = 0;
    };

    // As the list last set them, so a pass sets only those that differ. A new attachment descriptor holds no texture,
    // no resolve texture, mip 0 and slice 0; its actions and clear value are not assumed, so the list sets them all the
    // first time (`actionsKnown`).
    struct HeldAttachment
    {
        const MTL::Texture* texture = nullptr;
        uint32_t mip = 0;
        uint32_t layer = 0;
        bool actionsKnown = false; // The list has set the load action, store action and clear value.
        MTL::LoadAction load = MTL::LoadActionDontCare;
        MTL::StoreAction store = MTL::StoreActionStore;
        MTL::ClearColor clearColor{ 0.0, 0.0, 0.0, 0.0 };
        double clearDepth = 0.0;
        uint32_t clearStencil = 0;
        const MTL::Texture* resolve = nullptr;
    };

    // `beginRenderPass` copies the attachments and opens no encoder; the render encoder opens at the first command that
    // needs it (`ensureRender`). Until then the pass's state commands wait in the prefix, which the open replays in
    // order through the same filtered setters. Containers keep their capacity, so a pass allocates nothing once a list
    // has recorded one as large. Only the list's recording thread touches it.
    struct DrawState
    {
        std::vector<PassColor> colors;
        PassDepth depth;
        bool passOpen = false;
        // Every attachment then loads what the last encoder stored.
        bool resuming = false;
        // Their labels are the last entries of `ListState::scopeLabels`. They live on the render encoder, so a split
        // pops them and the resuming encoder pushes them again.
        size_t passGroups = 0;
        // Until the pass's first render encoder writes it; `core::kNoTimestamp` once written, and for a pass that
        // claimed no pair.
        uint32_t passBegin = core::kNoTimestamp;
        // Made at the list's first pass with alloc and init, kept for the list's life.
        MTL4::RenderPassDescriptor* descriptor = nullptr;
        uint32_t descriptorColors = 0;
        // Each retained, by index, as passes first used them, so configuring and emptying them calls none of the
        // descriptor's getters, which may autorelease.
        std::vector<MTL::RenderPassColorAttachmentDescriptor*> colorAttachments;
        MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = nullptr;     // Retained.
        MTL::RenderPassStencilAttachmentDescriptor* stencilAttachment = nullptr; // Retained.
        std::vector<HeldAttachment> colorHeld;                                   // At the same index as `colorAttachments`.
        HeldAttachment depthHeld;
        HeldAttachment stencilHeld;

        const PipelineState* pipeline = nullptr;
        // Kept across passes.
        ResolvedPipelines resolved;
        // The bound pipeline's list form, or what `setPrimitiveTopology` set since.
        MTL::PrimitiveType topology = MTL::PrimitiveTypeTriangle;
        // Set at once while the encoder is open, and by the replay of its prefix entry otherwise, so a pipeline
        // replayed before that entry applies the values in force there. Zero when the pass begins.
        DepthBias bias;
        RenderHeld held;                   // Metal's defaults at each open.
        std::vector<PrefixCommand> prefix; // Recorded since the render encoder closed, in order.
        // Per kind, one plus the index of that kind's entry in `prefix`, or 0: a later command of the kind replaces it,
        // since no draw between them could observe the first.
        std::array<uint32_t, kPrefixKinds> prefixLatest = {};

        // The root recorded last; 0 for none. Never in the prefix: a draw or dispatch writes it to the table when the table
        // holds another, and a counted draw of one record keeps the root it needs in its prefix entry.
        uint64_t root = 0;
        uint64_t tableRoot = 0; // What the list's table holds at `buffer(0)`, as this recording wrote it; 0 for none.
        // Only the list writes its table, so this holds across recordings. The zero record when the list is created.
        uint64_t tableDrawRecord = 0;
        // Fixed from the device's creation.
        uint64_t zeroRecord = 0;
        // A dispatch sets it again when the open compute encoder holds another; null when none is bound.
        const PipelineState* compute = nullptr;
        IcbPool icbs;
        // A counted mesh draw's commands bind them themselves. Fixed from the device's creation, so read once per list;
        // 0 before the first.
        uint64_t viewTableAddress = 0;
        uint64_t samplerTableAddress = 0;

        // Keeps the containers' capacity, the descriptor, the indirect command buffers, the record address the table
        // holds, the zero record and the table addresses.
        void reset();

        // `endRenderPass` also unbinds the pipeline.
        void endPass();

        // Merges with the entry of its kind recorded since the last open or barrier entry: that entry is replaced in
        // place, since nothing between them observes it.
        void record(const PrefixCommand& command);

        // For an entry that observes the state recorded before it, such as a counted draw's execute; no later entry
        // merges across it.
        void appendBarrierEntry(const PrefixCommand& command);

        void clearPrefix();

        // The caller holds an autorelease pool.
        void release(Residency& residency);

        // The descriptor's attachments hold strong references to the last pass's textures, so a list waiting in its
        // pool would keep a destroyed texture's memory alive. The setters go through the kept attachment descriptors,
        // so no getter runs and no pool opens; they are no message to a command buffer, encoder or argument table, so
        // they are not counted.
        void forgetTextures();
    };

    // Load operations are the caller's on the pass's first encoder and `Load` on one resuming it; a store other than
    // `Store` without a resolve is created as `MTL::StoreActionUnknown` for `finishPass` to set. A resuming encoder
    // restores the state the split encoder held and pushes the in-pass debug groups again, outermost first; the prefix
    // then replays in order.
    MTL4::RenderCommandEncoder* openRender(CommandListImpl& list);

    // At a split, `Store`, so the resuming encoder loads it, with a resolve for an attachment that has a resolve
    // target, as Metal requires there; else the caller's operation with its resolve.
    void finishPass(CommandListImpl& list, bool split);

    // It orders nothing: Metal 4 tracks no hazard between render encoders, so a caller that split merges a barrier from
    // `StageFragment` to `StageFragment` before the pass resumes, after any compute work it records in between, so that
    // work's opening barrier does not wait on the ended encoder's fragments.
    void splitPass(CommandListImpl& list);

    // The winding is set only when it decides something: the pipeline culls, tests stencil or has a fragment entry. The
    // depth bias is `DrawState::bias` when the pipeline has `RasterState::depthBias`, zero otherwise.
    void applyPipeline(CommandListImpl& list, const PipelineState& pipeline);

    void applyDepthBias(CommandListImpl& list, const DepthBias& bias);

    // Writes `address` at `buffer(0)` of the list's table unless the table holds it. Called before each command that
    // reads the table, never at `setRoot`, so an encoder that only executes writes none.
    void applyRoot(CommandListImpl& list, uint64_t address);

    // For the drawId of the next draw's vertex, task and mesh entries.
    void applyDrawRecord(CommandListImpl& list, uint64_t address);

    // Only where it differs from Metal's defaults.
    void restoreHeld(CommandListImpl& list, const RenderHeld& previous);

    void replayPrefix(CommandListImpl& list);

    // Inside an autorelease pool, because the validation layer autoreleases in a draw it checks. A one-record entry
    // binds the caller's record at `buffer(3)` first.
    void replayCounted(CommandListImpl& list, const PrefixCommand& command);

    // A pipeline is set only when the encoder holds another, as after a backend kernel or a new encoder.
    MTL4::ComputeCommandEncoder* applyComputePipeline(CommandListImpl& list);
} // namespace rojoRHI::metal4
