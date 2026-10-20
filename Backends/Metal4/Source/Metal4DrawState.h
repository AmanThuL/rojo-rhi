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

    /// The three values of a depth bias, as `setDepthBias` takes them.
    struct DepthBias
    {
        float constant = 0.0f;    ///< In units of the format's minimum resolvable depth difference.
        float slopeScaled = 0.0f; ///< Times the polygon's maximum depth slope.
        float clamp = 0.0f;       ///< The largest magnitude of the bias; zero for none.

        bool operator==(const DepthBias&) const = default;
    };

    /// What the open render encoder holds, as the list last set it; a new encoder holds Metal's defaults, which are these
    /// starting values. A setter sends Metal only a value that differs from the one held.
    struct RenderHeld
    {
        const PipelineState* pipeline = nullptr;                ///< The pipeline whose render state is set; null for none.
        const MTL::DepthStencilState* depthStencil = nullptr;   ///< The depth-stencil state; null is Metal's default.
        MTL::CullMode cull = MTL::CullModeNone;                 ///< The cull mode.
        MTL::Winding winding = MTL::WindingClockwise;           ///< The front-facing winding.
        MTL::TriangleFillMode fill = MTL::TriangleFillModeFill; ///< The triangle fill mode.
        MTL::DepthClipMode depthClip = MTL::DepthClipModeClip;  ///< The depth clip mode.
        DepthBias bias;                                         ///< The depth bias applied.
        uint32_t stencilReference = 0;                          ///< The stencil reference of both faces.
        std::array<float, 4> blendColor = {};                   ///< The blend color.
        bool viewportKnown = false;                             ///< A viewport is set; Metal's default is unknown here.
        MTL::Viewport viewport = {};                            ///< The viewport, when `viewportKnown`.
        bool scissorKnown = false;                              ///< A scissor rectangle is set.
        MTL::ScissorRect scissor = {};                          ///< The scissor rectangle, when `scissorKnown`.
    };

    /// The kinds of command a list records into its prefix while its pass's render encoder is closed.
    enum class PrefixKind : uint8_t
    {
        Pipeline,         ///< `setPipeline` with a graphics pipeline.
        Viewport,         ///< `setViewport`.
        Scissor,          ///< `setScissor`.
        StencilReference, ///< `setStencilReference`.
        BlendColor,       ///< `setBlendConstants`.
        DepthBias,        ///< `setDepthBias`.
        Root,             ///< `setRoot`, which writes the table's `buffer(0)`.
        Execute,          ///< A counted draw's indirect command buffer, run over the range its kernel wrote.
        DrawIndirect,     ///< A counted draw of one record: one native indirect draw of the record its kernel wrote.
        IndexedIndirect,  ///< A counted indexed draw of one record: one native indirect indexed draw.
        MeshExecute,      ///< A counted mesh draw's indirect command buffer, run over the range its kernel wrote.
        MeshIndirect,     ///< A counted mesh draw of one record: its record bound at `buffer(3)`, one native indirect mesh draw.
    };

    /// How many kinds of prefix command there are.
    inline constexpr size_t kPrefixKinds = 12;

    /// What an `Execute` or `MeshExecute` entry runs: an indirect command buffer and the address of the range a kernel writes for it.
    struct PrefixExecute
    {
        const MTL::IndirectCommandBuffer* buffer; ///< The indirect command buffer.
        uint64_t range;                           ///< The GPU address of its `MTL::IndirectCommandBufferExecutionRange`.
    };

    /// What a `DrawIndirect` or `IndexedIndirect` entry draws: the native record a kernel writes, the primitive type in force
    /// when the draw was recorded and, for an indexed draw, the index buffer.
    struct PrefixIndirect
    {
        uint64_t record;              ///< The GPU address of the `MTL::Draw*IndirectArguments` record.
        MTL::PrimitiveType primitive; ///< The primitive type.
        MTL::IndexType indexType;     ///< `IndexedIndirect`: the index type.
        uint64_t indices;             ///< `IndexedIndirect`: the GPU address of the index buffer.
        uint64_t indexLength;         ///< `IndexedIndirect`: the bytes of the index buffer.
    };

    /// What a `MeshIndirect` entry draws: the caller's record, which the draw binds at `buffer(3)`, the native record a
    /// kernel writes, and the pipeline bound when the draw was recorded, whose threadgroup sizes the draw takes.
    struct PrefixMesh
    {
        uint64_t record;               ///< The GPU address of the `DrawMeshTasksIndirectArgs` record.
        uint64_t native;               ///< The GPU address of the `MTL::DispatchThreadgroupsIndirectArguments` record.
        const PipelineState* pipeline; ///< The mesh pipeline.
    };

    /// One command of the prefix: its kind and the value the kind names.
    struct PrefixCommand
    {
        PrefixKind kind = PrefixKind::Root; ///< Which member of the union holds the value.
        union {
            const PipelineState* pipeline; ///< `Pipeline`: the graphics pipeline's payload.
            MTL::Viewport viewport;        ///< `Viewport`.
            MTL::ScissorRect scissor;      ///< `Scissor`.
            uint32_t stencilReference;     ///< `StencilReference`.
            std::array<float, 4> color;    ///< `BlendColor`.
            std::array<float, 3> bias;     ///< `DepthBias`: constant, slope-scaled and clamp.
            uint64_t root;                 ///< `Root`: the GPU address.
            PrefixExecute execute;         ///< `Execute` and `MeshExecute`.
            PrefixIndirect indirect;       ///< `DrawIndirect` and `IndexedIndirect`.
            PrefixMesh mesh;               ///< `MeshIndirect`.
        };
    };

    /// One color attachment of the open pass, copied at `beginRenderPass` with its native textures.
    struct PassColor
    {
        const MTL::Texture* texture = nullptr;       ///< The attachment's native texture.
        uint32_t mip = 0;                            ///< The mip it renders to.
        uint32_t layer = 0;                          ///< The layer it renders to; a cube face is a layer.
        LoadOp load = LoadOp::Load;                  ///< The caller's load operation.
        StoreOp store = StoreOp::Store;              ///< The caller's store operation.
        MTL::ClearColor clear{ 0.0, 0.0, 0.0, 0.0 }; ///< The clear color, used when `load` is `Clear`.
        const MTL::Texture* resolve = nullptr;       ///< The native resolve target, or null.
    };

    /// The depth attachment of the open pass, copied at `beginRenderPass` with its native texture; null when the pass has none.
    struct PassDepth
    {
        const MTL::Texture* texture = nullptr;    ///< The attachment's native texture, or null.
        uint32_t mip = 0;                         ///< The mip it renders to.
        uint32_t layer = 0;                       ///< The layer it renders to.
        LoadOp load = LoadOp::Load;               ///< The depth load operation.
        StoreOp store = StoreOp::Store;           ///< The depth store operation.
        double clearDepth = 0.0;                  ///< The clear depth.
        bool stencil = false;                     ///< The format has a stencil plane, which the stencil fields govern.
        LoadOp stencilLoad = LoadOp::DontCare;    ///< The stencil load operation.
        StoreOp stencilStore = StoreOp::DontCare; ///< The stencil store operation.
        uint32_t clearStencil = 0;                ///< The clear stencil value.
    };

    /// What a command list keeps of its open render pass and of the state its draws and dispatches read. `beginRenderPass`
    /// copies the attachments and opens no encoder; the render encoder opens at the first command that needs it
    /// (`ensureRender`), from the list's one render pass descriptor. Until then the pass's state commands wait in the prefix,
    /// which the open replays in order through the same filtered setters. The containers keep their capacity and the
    /// descriptor lives as long as the list, so a pass allocates nothing once a list has recorded one as large. Only the
    /// list's recording thread touches it.
    struct DrawState
    {
        std::vector<PassColor> colors; ///< The color attachments of the open pass, in order.
        PassDepth depth;               ///< The depth attachment of the open pass.
        bool passOpen = false;         ///< A render pass is open.
        /// The pass's render encoder has opened once, so the next one resumes it: every attachment loads what the last stored.
        bool resuming = false;
        /// The debug groups of the scopes open inside the pass, whose labels are the last entries of `ListState::scopeLabels`;
        /// they live on the render encoder, so a split pops them and the resuming encoder pushes them again.
        size_t passGroups = 0;
        /// The begin entry of the open pass's timestamp pair, until the pass's first encoder writes it at its start;
        /// `core::kNoTimestamp` once written, and for a pass that claimed no pair.
        uint32_t passBegin = core::kNoTimestamp;
        /// The list's render pass descriptor: made at its first pass with alloc and init, kept for the list's life.
        MTL4::RenderPassDescriptor* descriptor = nullptr;
        uint32_t descriptorColors = 0; ///< The color attachments `descriptor` last held a texture for.
        bool descriptorDepth = false;  ///< `descriptor` holds a depth texture, and a stencil one for a stencil format.

        /// The graphics pipeline bound last, as recorded; null when none is bound.
        const PipelineState* pipeline = nullptr;
        /// The primitive type of the next draw: the bound pipeline's list form, or what `setPrimitiveTopology` set since.
        MTL::PrimitiveType topology = MTL::PrimitiveTypeTriangle;
        /// The values of the last `setDepthBias` the render encoder has seen: set at once while it is open, and by the replay
        /// of its prefix entry otherwise, so a pipeline replayed before that entry applies the values in force there. Zero
        /// when the pass begins.
        DepthBias bias;
        RenderHeld held;                   ///< What the open render encoder holds; Metal's defaults at each open.
        std::vector<PrefixCommand> prefix; ///< The state commands recorded since the render encoder closed, in order.
        /// Per kind, one plus the index of that kind's entry in `prefix`, or 0 for none: a later command of the kind replaces
        /// it, since no draw lies between them that could observe the first. `appendBarrierEntry` clears it after an entry that
        /// observes the state, such as an execute.
        std::array<uint32_t, kPrefixKinds> prefixLatest = {};

        uint64_t root = 0;            ///< The root recorded last, whether the table holds it yet or it waits in the prefix; 0 for none.
        uint64_t tableRoot = 0;       ///< The address the list's table holds at `buffer(0)`, as this recording wrote it; 0 for none.
        uint64_t tableDrawRecord = 0; ///< The address the list's table holds at `buffer(3)`, as this recording wrote it; 0 for none.
        /// The four zero bytes a direct mesh draw binds at `buffer(3)`, reserved in the list's frame-arena page at the
        /// recording's first one; 0 until then.
        uint64_t zeroRecord = 0;
        /// The compute pipeline the caller bound, which a dispatch sets again when the open compute encoder holds another;
        /// null when none is bound.
        const PipelineState* compute = nullptr;
        /// The indirect command buffers of the list's counted draws and the residency set that keeps them resident.
        IcbPool icbs;
        /// The GPU addresses of the device's view and sampler tables, which a counted mesh draw's commands bind themselves:
        /// fixed from the device's creation, so read once per list and kept across recordings; 0 before the first.
        uint64_t viewTableAddress = 0;
        uint64_t samplerTableAddress = 0; ///< See `viewTableAddress`.

        /// Forgets everything the list recorded, as when the list is acquired again: the pass, the graphics state, the root and
        /// the addresses the list's table holds, the zero record and the compute pipeline, and frees every indirect command
        /// buffer slot for the new recording. Keeps the containers' capacity, the descriptor, the indirect command buffers and
        /// the view and sampler table addresses.
        void reset();

        /// Forgets the pass and the graphics state recorded in it, as `endRenderPass` does, which also unbinds the pipeline.
        void endPass();

        /// Appends `command` to the prefix, merging it with the entry of its kind recorded since the last open or the last
        /// barrier entry, if any: that entry is replaced in place, since nothing between them observes it.
        void record(const PrefixCommand& command);

        /// Appends `command`, an entry that observes the state recorded before it, such as a counted draw's execute, and
        /// clears `prefixLatest`, so no later entry merges across it.
        void appendBarrierEntry(const PrefixCommand& command);

        /// Empties the prefix after its replay, keeping its capacity.
        void clearPrefix();

        /// Releases the descriptor, the indirect command buffers and the list's residency set. The caller holds an autorelease
        /// pool.
        void release();

        /// Empties the descriptor's attachments, which hold strong references to the last pass's textures, so a list waiting
        /// in its pool keeps no destroyed texture's memory alive. Opens an autorelease pool only when the descriptor holds a
        /// texture. The setters are no message to a command buffer, encoder or argument table, so they are not counted.
        void forgetTextures();
    };

    /// Opens the open pass's render encoder and returns it; `ensureRender` calls it when no render encoder is open. The
    /// descriptor takes the copied attachments, with the caller's load operations on the pass's first encoder and `Load` on
    /// one resuming it, and the caller's store operation when it is `Store` without a resolve, else
    /// `MTL::StoreActionUnknown` for `finishPass` to set. An open compute encoder closes first. The new encoder takes the
    /// innermost open scope's label, binds the list's argument table to the four render stages and emits the pending
    /// barrier's render part; a resuming encoder sets again the state the encoder the split ended held and pushes the open
    /// in-pass debug groups again, outermost first. The prefix then replays in order. A pass must be open.
    MTL4::RenderCommandEncoder* openRender(CommandListImpl& list);

    /// Sets the store action of every attachment the open render encoder was created with `MTL::StoreActionUnknown` for: at a
    /// split (`split`) `Store`, so the resuming encoder loads it, with a resolve for an attachment that has a resolve target,
    /// as Metal requires there; else the caller's operation with its resolve.
    void finishPass(CommandListImpl& list, bool split);

    /// Splits the open pass when its render encoder is open: pops the in-pass debug groups from the encoder, innermost first,
    /// stores every attachment (`finishPass` with `split`) and ends the encoder, so the next `ensureRender` resumes the pass.
    /// Does nothing when the render encoder is not open.
    void splitPass(CommandListImpl& list);

    /// Sets `pipeline`'s render state on the open render encoder where it differs from what the encoder holds: the pipeline
    /// state, its depth-stencil state, its cull mode, its front-facing winding when the winding decides something (the
    /// pipeline culls, tests stencil or has a fragment entry; a depth-only or rasterization-off pipeline skips it), its fill
    /// and depth clip modes, then the depth bias, `DrawState::bias` when the pipeline has `RasterState::depthBias` and zero
    /// otherwise. Counts each call.
    void applyPipeline(CommandListImpl& list, const PipelineState& pipeline);

    /// Sets the depth bias `bias` on the open render encoder unless it holds it; counts the call.
    void applyDepthBias(CommandListImpl& list, const DepthBias& bias);

    /// Writes `address` to the list's table at `buffer(0)` unless the table holds it; counts the call.
    void applyRoot(CommandListImpl& list, uint64_t address);

    /// Writes `address`, a draw record or the zero record, to the list's table at `buffer(3)` unless the table holds it, for
    /// the task and mesh entries of the next mesh draw; counts the call.
    void applyDrawRecord(CommandListImpl& list, uint64_t address);

    /// Sets on a render encoder that has just opened the state `previous`, which the encoder a split ended held, where it
    /// differs from Metal's defaults; counts each call.
    void restoreHeld(CommandListImpl& list, const RenderHeld& previous);

    /// Replays the prefix in order on the render encoder that has just opened, through the filtered setters, then empties it.
    void replayPrefix(CommandListImpl& list);

    /// Runs on the open render encoder the counted draw `command`, an `Execute`, `DrawIndirect`, `IndexedIndirect`,
    /// `MeshExecute` or `MeshIndirect` entry of the prefix, inside an autorelease pool, because the validation layer
    /// autoreleases in a draw it checks; a `MeshIndirect` entry binds its record at `buffer(3)` first. Counts each call and
    /// marks the encoder's work in the stages the draw runs in: vertex and fragment, or object, mesh and fragment.
    void replayCounted(CommandListImpl& list, const PrefixCommand& command);

    /// Returns the compute encoder for a dispatch, opening it when none is open and emitting a barrier owed to the dispatch
    /// stage, with the caller's compute pipeline set: a pipeline is set only when the encoder holds another, as after a
    /// backend kernel or a new encoder. A compute pipeline must be bound.
    MTL4::ComputeCommandEncoder* applyComputePipeline(CommandListImpl& list);
} // namespace rojoRHI::metal4
