//----------------------------------------------------------------------------------------------------------------------
/// @file CommandList.h
/// @brief Declares the frame command recording interface and its barrier vocabulary.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Buffer.h"
#include "RHI/GpuAddress.h"
#include "RHI/RenderPass.h"
#include "RHI/Texture.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace lmx::rhi {

class ComputePipeline;
class GraphicsPipeline;
class Sampler;

/// Additional memory visibility a barrier must establish beyond ordinary access to one resource.
enum class BarrierOptions : uint8_t {
    None = 0,         ///< Orders ordinary accesses to one logical resource.
    ResourceAlias = 1 ///< Makes reused physical memory visible through a different resource.
};

/// Combines independent barrier visibility requirements.
constexpr BarrierOptions operator|(BarrierOptions a, BarrierOptions b) {
    return static_cast<BarrierOptions>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// Returns true when `options` includes `option`.
constexpr bool hasBarrierOption(BarrierOptions options, BarrierOptions option) {
    return (static_cast<uint8_t>(options) & static_cast<uint8_t>(option)) != 0;
}

/// Declares what a shader does with a storage binding, so that validation and a future backend's
/// state tracking know the intent without inspecting the shader. This backend does not track
/// resource state, so the declaration constrains only which resources may be bound where.
enum class StorageAccess {
    Read,     ///< The shader only reads the binding.
    Write,    ///< The shader only writes the binding.
    ReadWrite ///< The shader both reads and writes the binding.
};

/// The byte alignment CommandList::bindFrameData places a block at unless the caller asks for more.
///
/// It is the widest constant-buffer offset alignment the modelled backends require, so a block
/// placed at it is addressable as a constant buffer everywhere rather than only where the current
/// hardware happens to be lenient.
inline constexpr uint64_t kFrameDataAlignment = 256;

/// Records one frame's render, compute, and copy passes, bindings, barriers, dispatches, and draws.
///
/// Exactly one pass is open at a time: every command below documents the scope it is valid in, and
/// calling it outside that scope is a sequencing bug the backend asserts on rather than a failure
/// it reports. Bindings live in the frame's argument table and therefore survive across passes of
/// the frame; a pass that depends on a slot binds it rather than inheriting whatever an earlier
/// pass left there.
class CommandList {
public:
    /// Number of buffer slots in the shared argument-table buffer namespace.
    static constexpr uint32_t kMaxBufferBindings = 8;
    /// Number of texture slots in the argument-table texture namespace.
    static constexpr uint32_t kMaxTextureBindings = 16;
    /// Number of sampler slots in the argument-table sampler namespace.
    static constexpr uint32_t kMaxSamplerBindings = 8;

    /// Destroys the command list through its owning device.
    virtual ~CommandList() = default;
    /// Begins a render pass using the supplied attachments and load actions.
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    /// Begins a compute pass. Inside it, bindComputePipeline, bindStorageBuffer,
    /// bindStorageTexture, the read-only texture/sampler/buffer binds, bindFrameData, dispatch, and
    /// dispatchIndirect are valid; render-pass commands are not. `label` names the pass in GPU
    /// captures, validation diagnostics, and passTimings(); backends substitute a stable fallback
    /// for an empty one.
    /// Begins a labeled compute pass on this command list.
    virtual void beginComputePass(std::string_view label) = 0;
    /// Binds a compute pipeline for subsequent dispatches. Valid only inside a compute pass.
    virtual void bindComputePipeline(ComputePipeline& pipeline) = 0;
    /// Binds a buffer for shader reads and/or writes at the given argument-table buffer slot --
    /// the same index space bindBuffer and bindFrameData use. `access` declares what the shader
    /// does with it and must be granted by the buffer's BufferDesc storage flags. Valid only inside
    /// a compute pass. Ordering against other passes is not implied: a dispatch that must see an
    /// earlier pass's writes needs an explicit barrier.
    /// Binds a storage buffer with declared access to an argument-table buffer slot.
    virtual void bindStorageBuffer(uint32_t slot, Buffer& buffer, StorageAccess access) = 0;
    /// Binds a texture for shader reads and/or writes at the given argument-table texture slot --
    /// the same index space bindTexture uses. `view` selects the subresources and format the
    /// shader addresses, defaulting to the whole texture in its own format, and `access` declares
    /// what the shader does with them; both must be granted by the texture's TextureDesc storage
    /// flags and format. Valid only inside a compute pass. Like bindStorageBuffer, this implies no
    /// ordering: a dispatch that must see an earlier pass's writes needs an explicit barrier.
    /// Binds a storage texture view with declared access to an argument-table texture slot.
    virtual void bindStorageTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view,
                                    StorageAccess access) = 0;
    /// Dispatches a grid of threadgroups; each argument is a count of *threadgroups*, not of
    /// threads, and the threads within one come from the bound pipeline's threadsPerThreadgroup.
    /// Every count must be greater than zero. Valid only inside a compute pass, after
    /// bindComputePipeline. Dispatches within one pass carry no ordering guarantee at all --
    /// neighboring dispatches may overlap execution, and there is no barrier that orders them,
    /// because textureBarrier is valid only between passes. A dispatch that must see another's
    /// writes therefore belongs in a later pass: end this one, record the barrier, and begin the
    /// next.
    /// Records a compute dispatch of the given threadgroup counts.
    virtual void dispatch(uint32_t threadgroupsX, uint32_t threadgroupsY,
                          uint32_t threadgroupsZ) = 0;
    /// Dispatches the threadgroup counts a DispatchIndirectArgs sitting at `offset` bytes into
    /// `argumentBuffer` holds; threads per threadgroup still come from the bound pipeline, because
    /// the GPU-side struct carries counts only. Valid only inside a compute pass, after
    /// bindComputePipeline. `offset` must be a multiple of kIndirectArgsAlignment and the whole
    /// struct must fit in the buffer.
    ///
    /// The arguments are read by the GPU at execution, so whatever wrote them must be ordered
    /// against this dispatch: a CPU-filled buffer is ordered by the frame boundary, and a buffer an
    /// earlier pass wrote needs a bufferBarrier ending in BufferUse::IndirectArgument. Their values
    /// are never validated -- nothing on the CPU can read them -- so a count of zero is a dispatch
    /// that does nothing rather than the caller error dispatch() would report.
    /// Records a compute dispatch whose threadgroup counts the GPU reads from a buffer.
    virtual void dispatchIndirect(Buffer& argumentBuffer, uint64_t offset) = 0;
    /// Ends the active compute pass.
    virtual void endComputePass() = 0;

    /// Begins a copy pass. Inside it, copyBuffer, copyBufferToTexture, copyTextureToBuffer,
    /// copyTexture, and fillBuffer are valid, and nothing else is: a copy pass has no pipeline and
    /// no bindings, so binds, draws, and dispatches all belong to one of the other two scopes.
    /// `label` names the pass in GPU captures, validation diagnostics, and passTimings() exactly as
    /// the other two pass kinds' labels do, and a copy pass is timed like them.
    ///
    /// Copies within one pass carry no ordering guarantee against each other, for the same reason
    /// dispatch() states: ordering between two pieces of work is what a barrier between two passes
    /// expresses, and there is no in-pass barrier. Copies of disjoint ranges are the normal
    /// contents of one pass.
    /// Begins a labeled copy pass on this command list.
    virtual void beginCopyPass(std::string_view label) = 0;
    /// Copies `size` bytes between two buffers, or between two ranges of one buffer as long as the
    /// ranges do not overlap. Both ranges must lie inside their allocation. Valid only inside a
    /// copy pass.
    /// Copies a byte range from one buffer into another.
    virtual void copyBuffer(Buffer& source, uint64_t sourceOffset, Buffer& destination,
                            uint64_t destinationOffset, uint64_t size) = 0;
    /// Copies texels out of a buffer into one rectangle of one subresource. `layout` describes the
    /// buffer side -- where the texels start and how their rows are strided -- and `region` the
    /// texture side. The bytes the two together address must lie inside the buffer, and the region
    /// inside the addressed mip level. Valid only inside a copy pass.
    /// Copies buffer bytes into a texture subresource region.
    virtual void copyBufferToTexture(Buffer& source, const BufferTextureLayout& layout,
                                     Texture& destination, const TextureCopyRegion& region) = 0;
    /// The reverse, and the way an arbitrary mip level or array layer is read back: copy the
    /// subresource into a buffer created with BufferDesc.cpuReadback, then read that buffer once
    /// the GPU work has completed. Texture::readback covers only the whole of level zero, so this
    /// is the only path to any other subresource. Valid only inside a copy pass.
    /// Copies a texture subresource region into buffer bytes.
    virtual void copyTextureToBuffer(Texture& source, const TextureCopyRegion& region,
                                     Buffer& destination, const BufferTextureLayout& layout) = 0;
    /// Copies one subresource rectangle to another, in the same texture or a different one. The two
    /// regions must have the same extent -- a copy does not filter or rescale -- and the two
    /// textures the same format: reinterpreting one format's bits as another's is what a texture
    /// view is for. Overlapping regions of one subresource are a caller error. Valid only inside a
    /// copy pass.
    /// Copies a texture subresource region into another texture subresource region.
    virtual void copyTexture(Texture& source, const TextureCopyRegion& sourceRegion,
                             Texture& destination, const TextureCopyRegion& destinationRegion) = 0;
    /// Writes `value` into every one of the `size` bytes starting at `offset`. The value is a
    /// *byte*, not a word: filling with a 32-bit pattern is not something this expresses, and the
    /// use it exists for is clearing an accumulation buffer to zero. The range must lie inside the
    /// buffer and must not be empty. Valid only inside a copy pass.
    /// Fills a buffer range with a repeated byte value.
    virtual void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint8_t value) = 0;
    /// Ends the active copy pass.
    virtual void endCopyPass() = 0;
    /// Binds a graphics pipeline for subsequent draws.
    virtual void bindPipeline(GraphicsPipeline& pipeline) = 0;
    /// Binds a buffer at the given argument-table buffer slot -- vertex buffers, read here by
    /// bindless vertex-pulling (StructuredBuffer, indexed with SV_VertexID), and any other buffer
    /// a shader addresses directly. Buffer slots are their own index space, shared with
    /// bindFrameData below: slot 0 here and slot 0 in bindFrameData are the same binding, so two
    /// different resources must not be bound to the same slot index within one pass. Texture and
    /// sampler slots (bindTexture, bindSampler) are separate index spaces again -- slot 0 in any
    /// one of the three does not collide with slot 0 in either other. Valid inside a render or a
    /// compute pass.
    /// Binds a buffer to an argument-table buffer slot in the active pass.
    virtual void bindBuffer(uint32_t slot, Buffer& buffer) = 0;
    /// Binds a texture view for shader reads at the given argument-table texture slot. `view`
    /// defaults to every subresource in the texture's own format. Texture slots are their own index
    /// space -- slot 0 here and buffer slot 0 coexist. Valid inside a render or a compute pass; the
    /// texture must carry shader-read usage. This backend grants
    /// shader-read usage for sampled = true, storageRead = true, and cpuReadback = true
    /// descriptors (so a texture created for CPU readback may be sampled from).
    /// Binds a shader-readable texture to an argument-table texture slot.
    virtual void bindTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view = {}) = 0;
    /// Binds a sampler at the given argument-table sampler slot. Sampler slots are their own
    /// index space, like texture slots -- so slot 0 here coexists with texture slot 0 and buffer
    /// slot 0. Valid inside a render or a compute pass.
    /// Binds a sampler to an argument-table sampler slot.
    virtual void bindSampler(uint32_t slot, Sampler& sampler) = 0;
    /// Suballocates `size` bytes of frame-owned CPU-visible memory whose GPU address is a multiple
    /// of `alignment`, copies `data` into it, binds that address at the given argument-table buffer
    /// slot -- the same index space bindBuffer above binds into -- and returns it.
    ///
    /// `data` must be non-null, `size` non-zero, and `alignment` a power of two of at least
    /// kFrameDataAlignment. The bytes are captured at call time, so the caller may reuse or free
    /// its source immediately. Valid inside a render or a compute pass; a copy pass carries no
    /// bindings and rejects it. One call performs one allocation, one copy, and at most one
    /// backend address bind.
    ///
    /// The returned address names the copy and is valid only inside the frame that made it. It may
    /// be written into another block uploaded later in the *same* frame, which is how a caller
    /// composes one frame's data out of several blocks. It must not be kept for a later frame: the
    /// memory belongs to a frame-in-flight slot the backend recycles once the GPU retires that
    /// frame, so a stale address reads whatever the frame three later wrote there. Data that has to
    /// survive a frame boundary is a Buffer bound with bindBuffer instead, and this operation never
    /// takes ownership of caller storage.
    ///
    /// Running out of frame-owned memory is not a caller error and is not reported here: the
    /// backend grows the frame's arena, and failing to grow it is a fatal device-resource
    /// diagnostic, because command recording has no partial-frame failure to recover through.
    /// Copies a block into frame-owned memory and binds its GPU address to a buffer slot.
    virtual GpuAddress bindFrameData(uint32_t slot, const void* data, uint64_t size,
                                     uint64_t alignment) = 0;
    /// The same operation at kFrameDataAlignment, which is what a caller with no alignment
    /// requirement of its own wants. The default lives here rather than on the virtual above so
    /// that it cannot vary with the static type a caller holds or drift between backends.
    /// Copies a block into frame-owned memory at the default alignment and binds its GPU address.
    GpuAddress bindFrameData(uint32_t slot, const void* data, uint64_t size) {
        return bindFrameData(slot, data, size, kFrameDataAlignment);
    }
    /// A CPU pointer is source metadata, not a GPU-readable value block. Callers uploading the
    /// pointee use the untyped data/size overload or pass the pointee value itself.
    template <typename T>
    GpuAddress bindFrameData(uint32_t slot, T* const& value) = delete;
    /// The typed form, and the one production callers should reach for: it copies exactly
    /// sizeof(T) bytes of `value` and asks for whichever of kFrameDataAlignment and alignof(T) is
    /// larger, so a block whose type over-aligns itself is still placed correctly.
    ///
    /// `T` must be a non-pointer, trivially copyable value, because the block is memcpy'd into
    /// memory the GPU reads directly. It may hold values and GpuAddress fields. C++ cannot inspect
    /// aggregate members here, so the caller must also ensure `T` contains no CPU pointers,
    /// references, ownership-bearing objects, or vtables.
    /// Copies a trivially copyable value into frame-owned memory and binds its GPU address.
    template <typename T>
    GpuAddress bindFrameData(uint32_t slot, const T& value) {
        static_assert(!std::is_pointer_v<T>,
                      "bindFrameData: T must be a value block, not a CPU pointer");
        static_assert(std::is_trivially_copyable_v<T>,
                      "bindFrameData: T must be trivially copyable");
        constexpr uint64_t alignment =
            alignof(T) > kFrameDataAlignment ? alignof(T) : kFrameDataAlignment;
        return bindFrameData(slot, &value, sizeof(T), alignment);
    }
    /// Records a non-indexed draw.
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    /// Indexed draw. Indices are uint32 (the only index type this RHI models); the index buffer
    /// is any Buffer holding them -- Metal 4 consumes it per-draw by GPU address, so there is no
    /// separate index-buffer bind state. firstIndex is an element offset into the buffer.
    /// Records an indexed draw using 32-bit indices from `indexBuffer`.
    virtual void drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex = 0) = 0;
    /// Draws the DrawIndirectArgs sitting at `offset` bytes into `argumentBuffer`. Valid only
    /// inside a render pass, after bindPipeline. `offset` must be a multiple of
    /// kIndirectArgsAlignment and the whole struct must fit in the buffer; the argument values
    /// themselves are never validated, exactly as dispatchIndirect documents, and must be ordered
    /// against whatever wrote them the same way.
    /// Records a non-indexed draw whose arguments the GPU reads from a buffer.
    virtual void drawIndirect(Buffer& argumentBuffer, uint64_t offset) = 0;
    /// The indexed counterpart, taking the index buffer per draw exactly as drawIndexed does --
    /// the count, the first index, the base vertex, and the instancing all come from the
    /// DrawIndexedIndirectArgs in `argumentBuffer` instead of from arguments here.
    /// Records an indexed draw whose arguments the GPU reads from a buffer.
    virtual void drawIndexedIndirect(Buffer& indexBuffer, Buffer& argumentBuffer,
                                     uint64_t offset) = 0;
    /// Ends the active render pass.
    virtual void endRenderPass() = 0;
    /// Orders the given subresources of `texture` between two uses, and makes the writes of the
    /// earlier use visible to the later one. Valid only between passes.
    ///
    /// At least one side must be a write: two reads have no hazard to order. Which passes the
    /// barrier separates is positional -- everything encoded before this call is the producer, and
    /// the next pass to open is the consumer -- so a barrier recorded with no pass after it is a
    /// dependency nothing consumes and is a caller error.
    ///
    /// `range` states the subresources the dependency covers, which is what a caller declares and
    /// what a graph reasons about. A backend may synchronize more than the range asks for: this
    /// one does, because Metal 4 tracks no resource state and its barriers order pipeline stages
    /// rather than subresources, so every barrier is at least a whole-queue stage dependency.
    ///
    /// What a barrier does *not* cover is the other consumers. The dependency is scoped to the
    /// consuming pass's own stage class -- the kind of encoder the pass opens -- because that is
    /// the side a backend can only express in the terms of the pass it is emitting for; this one
    /// records it as the consuming encoder's first command, naming the producing stages and its
    /// own. Passes of one stage class are ordered among themselves, so one barrier serves every
    /// later consumer of that class; a consumer of a *different* class needs its own barrier, over
    /// the same subresources and the same producing use, however wide the first one's range was.
    /// A render graph derives its barriers from this rule, and a caller hand-encoding two passes
    /// of different kinds over one producer's output owes each of them a barrier.
    ///
    /// ResourceAlias additionally flushes memory being reused through a different logical resource.
    /// The `texture` argument is the resource the consumer pass will use; ordinary same-resource
    /// transitions leave `options` at None.
    /// Orders a texture's subresources between a producing and a consuming use.
    virtual void textureBarrier(Texture& texture, const TextureSubresourceRange& range,
                                TextureUse from, TextureUse to,
                                BarrierOptions options = BarrierOptions::None) = 0;
    /// Same, for the whole texture -- the common case, and what a pass that declares no
    /// subresource detail means.
    /// Orders a whole texture between a producing and a consuming use.
    void textureBarrier(Texture& texture, TextureUse from, TextureUse to,
                        BarrierOptions options = BarrierOptions::None) {
        textureBarrier(texture, TextureSubresourceRange{}, from, to, options);
    }
    /// The same contract for a buffer: the range, the at-least-one-write rule, the positional
    /// producer and consumer, and the backend's freedom to synchronize more than the range asks for
    /// all read exactly as textureBarrier's do. It exists because a buffer hazard has no texture to
    /// borrow -- a fillBuffer whose zeros an accumulating dispatch must see, or arguments a compute
    /// pass writes for a later indirect draw, are dependencies on bytes, and expressing them
    /// through some unrelated texture's edge would be a lie a graph would later reason from.
    /// ResourceAlias has the same meaning as on textureBarrier: `buffer` is the resource receiving
    /// physical memory that a different logical resource used earlier.
    /// Orders a buffer's byte range between a producing and a consuming use.
    virtual void bufferBarrier(Buffer& buffer, const BufferRange& range, BufferUse from,
                               BufferUse to, BarrierOptions options = BarrierOptions::None) = 0;
    /// Same, for the whole buffer -- the common case, and what a pass that declares no byte-range
    /// detail means.
    /// Orders a whole buffer between a producing and a consuming use.
    void bufferBarrier(Buffer& buffer, BufferUse from, BufferUse to,
                       BarrierOptions options = BarrierOptions::None) {
        bufferBarrier(buffer, BufferRange{}, from, to, options);
    }
};

} // namespace lmx::rhi
