//----------------------------------------------------------------------------------------------------------------------
/// @file CommandList.h
/// @brief Declares the command list object handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>
#include <rojoRHI/BufferBarrier.h>
#include <rojoRHI/BufferTextureLayout.h>
#include <rojoRHI/ClearValue.h>
#include <rojoRHI/Geometry.h>
#include <rojoRHI/GlobalBarrier.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/IndexType.h>
#include <rojoRHI/PrimitiveTopology.h>
#include <rojoRHI/RenderPassDesc.h>
#include <rojoRHI/ResourceAccess.h>
#include <rojoRHI/SubresourceRange.h>
#include <rojoRHI/TextureBarrier.h>
#include <rojoRHI/TextureRegion.h>
#include <rojoRHI/UpscaleParams.h>
#include <rojoRHI/Viewport.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace rojoRHI {
    /// A recorded sequence of commands for one queue type. It wraps one backend pointer: copies refer to the same list, and a
    /// default-constructed one refers to none (VU-rojo-core-0004). It comes from a per-frame pool and returns to it when its
    /// frame retires; using it after that is undefined behaviour no tier detects. It is used only with the device that
    /// acquired it (VU-rojo-core-0005). One thread records one list at a time. In Release builds recording takes no locks,
    /// except when a list claims a new frame-arena page for `uploadFrameData` or for the label of a debug scope; a Debug-tier
    /// check may take the lock of the pool it reads (ADR 0020).
    ///
    /// Every command other than `end` is recorded on a list that is recording (VU-rojo-cmd-0063).
    class CommandList
    {
      public:
        /// Ends recording; the list then records nothing further and is ready for `Queue::submit`. The list is recording
        /// (VU-rojo-cmd-0002), no render pass is open (VU-rojo-cmd-0003) and every scope begun on it has ended
        /// (VU-rojo-cmd-0004). Thread class command list.
        void end();

        /// Copies `bytes` into the open frame's arena at an address aligned to `alignment` and returns that address, valid
        /// only within the frame that produced it. `bytes` is not empty (VU-rojo-buffer-0015), `alignment` is a power of two
        /// (VU-rojo-buffer-0016), a frame is open (VU-rojo-buffer-0017) and only work submitted before that frame's
        /// `Device::endFrame` reads the address; a command that takes the address is recorded on a list of the frame that
        /// produced it (VU-rojo-buffer-0020). Thread class command list.
        Addr uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);

        /// Begins a render pass: performs the load operations and no layout transition, forms a scope labeled `desc.label` and
        /// starts the dynamic state as chapter 06 lists it, with no pipeline bound. Recorded on a `Graphics` list with no
        /// render pass open (VU-rojo-cmd-0016); `desc` obeys VU-rojo-cmd-0005 to 0015. Thread class command list.
        void beginRenderPass(const RenderPassDesc& desc);

        /// Ends the open render pass: performs the store operations and resolves, closes the pass's scope and unbinds the
        /// pipeline. A render pass is open (VU-rojo-cmd-0018) and every scope begun inside it has ended (VU-rojo-cmd-0019).
        /// Thread class command list.
        void endRenderPass();

        /// Opens a debug scope labeled `label`, inside or outside a render pass; scopes nest. Thread class command list.
        void beginScope(std::string_view label);

        /// Closes the innermost open scope. A scope is open (VU-rojo-cmd-0032) and the innermost one began on the same side of
        /// the render pass boundary as this call (VU-rojo-cmd-0033). Thread class command list.
        void endScope();

        /// Binds a graphics pipeline inside a render pass or a compute pipeline outside one; binding a graphics pipeline resets
        /// the topology to the list form of its class. `pipeline` is live (VU-rojo-cmd-0020); a graphics pipeline is bound
        /// inside a render pass of a `Graphics` list and matches the pass's attachment formats and sample count
        /// (VU-rojo-cmd-0021); a compute pipeline is bound outside a render pass of a `Graphics` or `Compute` list
        /// (VU-rojo-cmd-0022). Thread class command list.
        void setPipeline(Pipeline pipeline);

        /// Sets the single viewport. Recorded inside a render pass (VU-rojo-pipeline-0015); `viewport` obeys
        /// VU-rojo-pipeline-0016 and 0017. Thread class command list.
        void setViewport(const Viewport& viewport);

        /// Sets the single scissor rectangle. Recorded inside a render pass (VU-rojo-pipeline-0018); `rect` lies within the
        /// pass's area (VU-rojo-pipeline-0019). Thread class command list.
        void setScissor(const Rect2D& rect);

        /// Sets the stencil reference for both faces. Recorded inside a render pass (VU-rojo-pipeline-0020). Thread class command
        /// list.
        void setStencilReference(uint8_t reference);

        /// Sets the color `BlendFactor::Constant` reads. Recorded inside a render pass (VU-rojo-pipeline-0021). Thread class
        /// command list.
        void setBlendConstants(float r, float g, float b, float a);

        /// Sets the exact topology of the following draws. Recorded inside a render pass with a graphics pipeline bound that has
        /// a vertex stage (VU-rojo-pipeline-0022); `topology` belongs to that pipeline's class (VU-rojo-pipeline-0023). Thread
        /// class command list.
        void setPrimitiveTopology(PrimitiveTopology topology);

        /// Sets the depth bias added to each fragment's depth. Recorded inside a render pass (VU-rojo-pipeline-0024) with a
        /// graphics pipeline bound that was created with `RasterState::depthBias` (VU-rojo-pipeline-0025). Thread class command
        /// list.
        void setDepthBias(float constant, float slopeScaled, float clamp);

        /// Binds `root` as the root block of every later draw and dispatch recorded on this list, until the next `setRoot` or
        /// `pushRoot`; only the address is captured. `root` is not null (VU-rojo-abi-0001), names a live buffer and lies
        /// inside it (VU-rojo-buffer-0009), and its byte offset in that buffer is a multiple of 256 (VU-rojo-abi-0002). The
        /// buffer stays alive until every draw and dispatch that reads the root completes, or the root is frame data of the
        /// frame being recorded (VU-rojo-abi-0003, checked by the validation tracker). Thread class command list.
        void setRoot(Addr root);

        /// Copies `value` into the open frame's arena at 256-byte alignment, binds the copy as `setRoot` does and returns its
        /// address, valid only within the current frame. It is `uploadFrameData` of the bytes of `value` followed by `setRoot`
        /// of the result, with the valid usage of both. `T` is trivially copyable and standard-layout and at most 65536
        /// bytes; the build-time layout check rejects a root type that breaks the root layout rules. Thread class command
        /// list.
        template<class T> Addr pushRoot(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "a root type is trivially copyable");
            static_assert(std::is_standard_layout_v<T>, "a root type is standard-layout");
            static_assert(sizeof(T) <= 65536, "a root block is at most 65536 bytes");
            const Addr address = uploadFrameData(std::as_bytes(std::span(&value, 1)), 256);
            setRoot(address);
            return address;
        }

        /// Draws `instances` instances of `vertices` vertices with the current topology; the shader pulls its vertices. A
        /// zero count records nothing. Recorded inside a render pass with a graphics pipeline that has a vertex stage bound
        /// (VU-rojo-cmd-0023), and with a viewport and a scissor set since `beginRenderPass` (VU-rojo-cmd-0024). Thread class
        /// command list.
        void draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);

        /// Draws `count` indices of `type` read from `indices` starting at index `firstIndex`, each added to `baseVertex`.
        /// Recorded as `draw` is, with a viewport and a scissor set (VU-rojo-cmd-0025). `indices` is live
        /// (VU-rojo-core-0001 to 0003), was created with `BufferUsage::Index`, and `firstIndex + count` indices of `type` lie
        /// within it (VU-rojo-cmd-0026). Thread class command list.
        void drawIndexed(
            Buffer indices, IndexType type, uint32_t count, uint32_t instances = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0
        );

        /// Launches an `x` by `y` by `z` grid of task threadgroups, or of mesh threadgroups when the pipeline has no task
        /// stage. Recorded inside a render pass with a graphics pipeline that has a mesh stage bound, and with a viewport and
        /// a scissor set (VU-rojo-cmd-0027). `x`, `y` and `z` are each at most `Limits::maxMeshTaskGroups` and their product
        /// at most `Limits::maxMeshTaskGroupsTotal` (VU-rojo-cmd-0028). Thread class command list.
        void drawMeshTasks(uint32_t x, uint32_t y, uint32_t z);

        /// Launches an `x` by `y` by `z` grid of threadgroups of the bound compute pipeline. Recorded outside a render pass
        /// on a `Graphics` or `Compute` list (VU-rojo-cmd-0029) with a compute pipeline bound since the list began or since
        /// the last `endRenderPass` (VU-rojo-cmd-0030); `x`, `y` and `z` are each at most `Limits::maxDispatchGroups`
        /// (VU-rojo-cmd-0031). Thread class command list.
        void dispatch(uint32_t x, uint32_t y, uint32_t z);

        /// Issues up to `maxCount` non-indexed draws whose `DrawIndirectArgs` records start at `args`, with the bound
        /// pipeline and root; a null `count` means exactly `maxCount`, otherwise it names one `uint32` read at execution.
        /// Recorded inside a render pass with a graphics pipeline that has a vertex stage bound (VU-rojo-indirect-0001), after
        /// a viewport and a scissor were set (VU-rojo-indirect-0021); `args` is non-null and 4-byte aligned and `maxCount` records lie in one buffer created
        /// with `BufferUsage::IndirectArgs` (VU-rojo-indirect-0002); `maxCount` is above zero and at most `Limits::maxDrawIndirectCount`
        /// (VU-rojo-indirect-0003); a non-null `count` is 4-byte aligned and lies in such a buffer (VU-rojo-indirect-0004). Each address names a live buffer
        /// and lies inside it (VU-rojo-buffer-0009). Barriers for GPU writes to the records or the count are checked by the validation tracker
        /// (VU-rojo-indirect-0005). Thread class command list.
        void drawIndirect(Addr args, uint32_t maxCount, Addr count = {});

        /// Issues up to `maxCount` indexed draws whose `DrawIndexedIndirectArgs` records start at `args`, fetching indices of
        /// `type` from `indices`. Recorded as `drawIndirect` is (VU-rojo-indirect-0006, 0022); `indices` is live
        /// (VU-rojo-core-0001 to 0003) and was created with `BufferUsage::Index` (VU-rojo-indirect-0007); `args` obeys
        /// VU-rojo-indirect-0008, `maxCount` VU-rojo-indirect-0009 and `count` VU-rojo-indirect-0010, with the barriers of
        /// VU-rojo-indirect-0011 checked by the validation tracker. Thread class command list.
        void drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count = {});

        /// Issues up to `maxCount` mesh-task draws whose `DrawMeshTasksIndirectArgs` records start at `args`. Recorded inside
        /// a render pass with a graphics pipeline that has a mesh stage bound (VU-rojo-indirect-0012), after a viewport and a
        /// scissor were set (VU-rojo-indirect-0023), on a device that
        /// reports `Capabilities::meshTasksIndirect` (VU-rojo-indirect-0013); `args` obeys VU-rojo-indirect-0014, `maxCount`
        /// VU-rojo-indirect-0015 and `count` VU-rojo-indirect-0016, with the barriers of VU-rojo-indirect-0017 checked by the
        /// validation tracker. Thread class command list.
        void drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count = {});

        /// Dispatches the bound compute pipeline with the threadgroup counts of the `DispatchIndirectArgs` record at `args`.
        /// Recorded outside a render pass with a compute pipeline bound (VU-rojo-indirect-0018); `args` is non-null and
        /// 4-byte aligned and the record lies in a buffer created with `BufferUsage::IndirectArgs`
        /// (VU-rojo-indirect-0019); the barrier of VU-rojo-indirect-0020 is checked by the validation tracker. Thread class
        /// command list.
        void dispatchIndirect(Addr args);

        /// Copies `size` bytes from `src` at `srcOffset` to `dst` at `dstOffset`, outside a render pass
        /// (VU-rojo-cmd-0042). `size` is above zero, both ranges lie within their buffers and the offsets and size are
        /// multiples of 4 (VU-rojo-cmd-0043); when `src` and `dst` are one buffer the ranges do not overlap
        /// (VU-rojo-cmd-0044). `src` is `Gpu` or `Upload` and `dst` is `Gpu` or `Readback` (VU-rojo-buffer-0001, 0002). Thread
        /// class command list.
        void copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);

        /// Copies texel data laid out by `layout` in `src` into `region` of `dst`, outside a render pass (VU-rojo-cmd-0046).
        /// `region` obeys VU-rojo-cmd-0038 to 0041 and `layout` VU-rojo-cmd-0034 to 0037; the bytes `layout` addresses lie
        /// within `src` (VU-rojo-cmd-0064), which is `Gpu` or `Upload` (VU-rojo-buffer-0002); `dst` has one sample
        /// (VU-rojo-cmd-0066). The layout of the subresources is checked by the validation tracker (VU-rojo-cmd-0067). Thread
        /// class command list.
        void copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);

        /// Copies `region` of `src` into `dst`, laid out by `layout`, outside a render pass (VU-rojo-cmd-0049). The rules of
        /// `copyBufferToTexture` apply to `region` and `layout`; the bytes `layout` addresses lie within `dst`
        /// (VU-rojo-cmd-0065), which is `Gpu` or `Readback` (VU-rojo-buffer-0001); `src` has one sample (VU-rojo-cmd-0068).
        /// The layout of the subresources is checked by the validation tracker (VU-rojo-cmd-0069). Thread class command list.
        void copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);

        /// Copies texels between textures of one format, or of two formats that differ only in sRGB transfer, outside a
        /// render pass (VU-rojo-cmd-0052). Both regions obey VU-rojo-cmd-0038 to 0041; the formats are copy-compatible, both
        /// textures have one sample and the regions have equal extents in texel blocks and equal layer counts
        /// (VU-rojo-cmd-0053); within one texture the regions share no subresource (VU-rojo-cmd-0055). The layouts are checked
        /// by the validation tracker (VU-rojo-cmd-0054). Thread class command list.
        void copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);

        /// Writes `pattern`, repeated in little-endian byte order, over `size` bytes of `dst` from `offset`. Recorded outside a
        /// render pass on a `Graphics` or `Compute` list (VU-rojo-cmd-0056); `size` is above zero, `offset` and `size` are
        /// multiples of 4 and the range lies within `dst` (VU-rojo-cmd-0057); `dst` was created with
        /// `BufferUsage::ShaderWrite` (VU-rojo-cmd-0058). Thread class command list.
        void fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern);

        /// Clears every subresource in `range` to `value`. Recorded outside a render pass on a `Graphics` or `Compute` list
        /// (VU-rojo-cmd-0060); `value` obeys VU-rojo-cmd-0059; `range` obeys VU-rojo-texture-0016, 0017 and 0041; `texture`
        /// was created with a usage VU-rojo-cmd-0061 allows. The layouts are checked by the validation tracker
        /// (VU-rojo-cmd-0070). Thread class command list.
        void clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value);

        /// Records one batch of barriers: all source scopes complete, then all layout transitions, then all destination scopes
        /// may begin. An empty batch records nothing. Recorded outside a render pass (VU-rojo-sync-0019); no two entries of
        /// `textures` cover the same subresource (VU-rojo-sync-0020). Each barrier obeys the rules of its descriptor. The
        /// ordering and layout rules are checked by the validation tracker (VU-rojo-sync-0021, VU-rojo-sync-0024). Thread
        /// class command list.
        void barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers = {}, std::span<const TextureBarrier> textures = {});

        /// Records one reconstruction by `upscaler`: it reads the inputs and its history and writes `params.output`, as compute
        /// work, and forms its own debug group named `params.label`, defaulted to `rojorhi.upscale.unnamed`. Recorded on a
        /// `Graphics` list (VU-rojo-upscale-0006) outside a render pass (VU-rojo-upscale-0007), at most once per upscaler per
        /// frame (VU-rojo-upscale-0008). The textures obey VU-rojo-upscale-0009 to 0014 and `params` obeys
        /// VU-rojo-upscale-0004, 0005 and 0015; none is a swapchain image (VU-rojo-present-0012). The layouts are checked by
        /// the validation tracker (VU-rojo-upscale-0016). Thread class command list.
        void upscale(Upscaler upscaler, const UpscaleParams& params);

        /// Declares shader accesses for the next draw, indexed draw, mesh-task draw or dispatch recorded on the list,
        /// including their indirect forms; several calls before one command accumulate. Each entry obeys VU-rojo-diag-0001 to
        /// 0003 and 0005; the list is recording (VU-rojo-diag-0004); each `stage` is one the next command runs
        /// (VU-rojo-diag-0006) and a draw or dispatch follows before `end` (VU-rojo-diag-0007). The rules are checked only
        /// on a device created with `DeviceDesc::validation`, which returns at once otherwise. In a Release build this is an
        /// empty inline function, so the consumer and the library must be built with the same NDEBUG setting. Thread class command
        /// list.
        void declareAccess(std::span<const ResourceAccess> accesses)
        {
#ifndef NDEBUG
            declareAccessChecked(accesses);
#else
            (void)accesses;
#endif
        }

      private:
        /// The library's side of `declareAccess`; it is defined in every build, and only a non-Release caller reaches it.
        void declareAccessChecked(std::span<const ResourceAccess> accesses);

        friend struct CommandListImpl;
        struct CommandListImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
