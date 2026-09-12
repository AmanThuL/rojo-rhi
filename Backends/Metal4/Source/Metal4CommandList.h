//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommandList.h
/// @brief Declares the Metal 4 command list and its per-frame transient binding state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "Metal4Common.h"
#include "Metal4FrameArena.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/GpuAddress.h"
#include "RHI/RenderPass.h"
#include "RHI/Texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lmx::rhi::metal4 {

class Metal4ComputePipeline;
struct Metal4TemporalScalerState;

// How many passes of one frame carry timestamps, counting every pass kind: a fixed per-frame
// budget that a real frame is expected to stay under, and whose exhaustion is a hard error rather
// than a silently dropped measurement.
inline constexpr uint32_t kMaxTimedPassesPerFrame = 64;

// Two timestamps per pass: one before its encoder opens, one after it closes.
inline constexpr uint32_t kTimestampsPerFrame = kMaxTimedPassesPerFrame * 2;

// One frame slot's GPU timestamps, owned by Metal4Device and written by the command list.
//
// The heap rotates on the same recycle discipline as the command allocators and frame-data arenas:
// the GPU writes into it for as long as the frame is in flight, so its entries may be read only
// after the frame-pacing event proves that frame retired, and are invalidated before the slot is
// handed to a new frame.
struct Metal4FrameTimestamps {
    // The counter heap is a Metal object, so its release has to happen inside an autorelease pool
    // (destructor rule in Metal4Common.h). The destructor lives on this struct rather than in
    // ~Metal4Device because the SharedPtr does: the type that declares the ownership is the one
    // that states how it ends, and no slot can be dropped elsewhere without the pool.
    ~Metal4FrameTimestamps();

    NS::SharedPtr<MTL4::CounterHeap> heap;
    // The frame's pass labels in encode order. Pass i owns heap entries 2i and 2i + 1, so this
    // array's size is also the write cursor -- there is no separate counter to keep in step.
    std::vector<std::string> passLabels;
    // The frame whose timestamps are sitting in the heap; zero means no frame has written it yet.
    // Written by endFrame and read only by the device's resolve, which requires the pacing event to
    // have signalled at least this value.
    uint64_t frameNumber = 0;
};

// The recording half of the frame protocol. One instance is owned by Metal4Device and handed
// back from every beginFrame(); it records into the device's single MTL4::CommandBuffer, which
// beginFrame has already re-opened against this frame's allocator.
//
// It owns nothing that outlives a frame. The command buffer, the argument table and the frame-data
// arena belong to the device (which outlives every command list it hands out), so all are held as
// raw pointers; only the pass encoders -- each created and destroyed inside a single
// begin/end pair -- are reference-counted here, because renderCommandEncoder() and
// computeCommandEncoder() return autoreleased (+0) objects that must survive the local autorelease
// pool they were created in.
//
// Those per-frame pointers are *not* fixed at construction: the device owns one argument table,
// one frame-data arena and one timestamp slot per frame in flight and hands this list the current
// frame's set through resetForFrame, so none is ever written outside the frame that owns it.
// endFrameReset nulls them again at commit, which is what lets beginRenderPass's assert
// distinguish "no frame is open" from "a frame is open" at all -- without it a stale pointer would
// keep every check passing while the writes landed in a slot the GPU was still reading.
//
// Encoder-scoped calls (bindPipeline, bindComputePipeline, bindBuffer, bindStorageBuffer,
// bindTexture, bindSampler, bindFrameData, draw, drawIndexed, dispatch) assert rather than return
// errors: calling them outside a pass is a sequencing bug, and the RHI CommandList methods return
// void.
// They also hold no autorelease pool of their own: none of them invokes an autoreleasing
// selector, so a per-call pool bought nothing and cost a create/drain on the hottest path in
// the backend. Verified against the vendored headers rather than assumed: bindPipeline, bindBuffer,
// draw are direct setters and draws on an already-retained encoder; bindFrameData's whole call
// path -- a bump allocation against the arena's precomputed CPU/GPU bases, a memcpy, and
// MTL4::ArgumentTable::setAddress() -- touches no Metal accessor at all, only the one scalar- and
// void-returning sendMessage that bind performs; drawIndexed adds only MTL::Buffer::length() and
// gpuAddress() (scalar sends) plus drawIndexedPrimitives (a void send) on top of plain C++
// Metal4Buffer::handle() getter; bindTexture is MTL::Texture::usage() and gpuResourceID()
// (scalar sends, MTLTexture.hpp:293 and :241) into MTL4::ArgumentTable::setTexture() (a void
// send, MTL4ArgumentTable.hpp:80); bindSampler is MTL::SamplerState::gpuResourceID() (a scalar
// send, MTLSampler.hpp:337) into MTL4::ArgumentTable::setSamplerState() (a void send,
// MTL4ArgumentTable.hpp:179). So no path produces a +0 object.
// bindStorageBuffer and bindStorageTexture add only scalar sends of the same kind, and
// bindStorageTexture's view lookup creates its own pool on the one path that allocates a view.
// textureBarrier and bufferBarrier accumulate queue stages and touch Metal not at all; the barrier
// they defer is MTL4::CommandEncoder::barrierAfterQueueStages(), also a void send, encoded inside
// the opening pass's existing pool. The copy commands are the same story again -- the copy and fill
// selectors are void sends on an already-retained encoder, over gpuAddress()/length() scalar sends.
//
// Three pools are load-bearing, in beginRenderPass, beginComputePass and beginCopyPass: the encoder
// factories are the only selectors here that return +0. The matching end calls keep a pool too
// (symmetry, and cheap insurance against a teardown path that starts autoreleasing) but none is
// *currently* carrying anything -- endEncoding() is a void sendMessage and the encoder reset is a
// plain release.
class Metal4CommandList final : public CommandList {
public:
    explicit Metal4CommandList(MTL4::CommandBuffer* commandBuffer)
        : m_commandBuffer(commandBuffer) {}

    Metal4CommandList(const Metal4CommandList&) = delete;
    Metal4CommandList& operator=(const Metal4CommandList&) = delete;

    void temporalScale(TemporalScaler& scaler, const TemporalScaleParams& params) override;
    void beginRenderPass(const RenderPassDesc& desc) override;
    void beginComputePass(std::string_view label) override;
    void bindComputePipeline(ComputePipeline& pipeline) override;
    void bindStorageBuffer(uint32_t slot, Buffer& buffer, StorageAccess access) override;
    void bindStorageTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view,
                            StorageAccess access) override;
    void dispatch(uint32_t threadgroupsX, uint32_t threadgroupsY, uint32_t threadgroupsZ) override;
    void dispatchIndirect(Buffer& argumentBuffer, uint64_t offset) override;
    void endComputePass() override;
    void beginCopyPass(std::string_view label) override;
    void copyBuffer(Buffer& source, uint64_t sourceOffset, Buffer& destination,
                    uint64_t destinationOffset, uint64_t size) override;
    void copyBufferToTexture(Buffer& source, const BufferTextureLayout& layout,
                             Texture& destination, const TextureCopyRegion& region) override;
    void copyTextureToBuffer(Texture& source, const TextureCopyRegion& region, Buffer& destination,
                             const BufferTextureLayout& layout) override;
    void copyTexture(Texture& source, const TextureCopyRegion& sourceRegion, Texture& destination,
                     const TextureCopyRegion& destinationRegion) override;
    void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint8_t value) override;
    void endCopyPass() override;
    void bindPipeline(GraphicsPipeline& pipeline) override;
    void bindBuffer(uint32_t slot, Buffer& buffer) override;
    void bindTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view) override;
    void bindSampler(uint32_t slot, Sampler& sampler) override;
    GpuAddress bindFrameData(uint32_t slot, const void* data, uint64_t size,
                             uint64_t alignment) override;
    // The non-virtual convenience and typed overloads would otherwise be hidden by the override
    // above for anything holding this concrete type; production callers hold a CommandList&, but a
    // backend-side caller should not have to know that to reach the default alignment.
    using CommandList::bindFrameData;
    void draw(uint32_t vertexCount, uint32_t firstVertex) override;
    void drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex) override;
    void drawIndirect(Buffer& argumentBuffer, uint64_t offset) override;
    void drawIndexedIndirect(Buffer& indexBuffer, Buffer& argumentBuffer, uint64_t offset) override;
    void endRenderPass() override;
    void textureBarrier(Texture& texture, const TextureSubresourceRange& range, TextureUse from,
                        TextureUse to, BarrierOptions options) override;
    void bufferBarrier(Buffer& buffer, const BufferRange& range, BufferUse from, BufferUse to,
                       BarrierOptions options) override;

    // beginFrame's half of the per-frame rotation: point this command list at the frame's
    // argument table, data arena and timestamp slot. `frameArena` is the slot's arena, already
    // reset under the same retirement proof, and `timestamps` is the slot whose heap the device has
    // already invalidated and whose label array it has already cleared. `tally` is device-lifetime
    // rather than per-frame, and is threaded through here so that endFrameReset drops it with
    // everything else. Must be called before any encoding in the frame; asserts no pass is open.
    void resetForFrame(MTL4::ArgumentTable* argumentTable, Metal4FrameArena* frameArena,
                       Metal4FrameDataTally* tally, Metal4FrameTimestamps* timestamps,
                       std::vector<std::shared_ptr<Metal4TemporalScalerState>>* temporalScalers);

    // endFrame's half: forget the frame's table once its work is committed, so that
    // encoding after endFrame fails our assert rather than quietly writing a slot the GPU owns.
    // Also where an unconsumed textureBarrier is caught -- see the .cpp.
    void endFrameReset();

    // True between beginRenderPass and endRenderPass. Metal4Device checks it so that ending a
    // frame with an open encoder is reported here rather than as a Metal abort at commit time.
    bool inRenderPass() const { return static_cast<bool>(m_encoder); }

    // The same for a compute pass. Kept separate from inRenderPass so the device's diagnostic can
    // name the pass kind the caller left open.
    bool inComputePass() const { return static_cast<bool>(m_computeEncoder); }

    // And for a copy pass. Metal 4 records copies on a compute encoder -- there is no MTL4 blit
    // encoder -- but the RHI scope is its own, so the two encoders are held separately and this
    // predicate is what keeps "dispatch inside a copy pass" a reported bug rather than a silent
    // success.
    bool inCopyPass() const { return static_cast<bool>(m_copyEncoder); }

    // True when a barrier has been declared but no consumer pass has opened yet.
    bool hasPendingBarrier() const { return m_pendingBarrierStages != MTL::Stages{}; }

    // Backend-internal, the same role handle() plays on every resource wrapper: a sibling Metal 4
    // file reaches the native objects through them, and the RHI CommandList interface has neither.
    // Metal4ImGui needs both, because Dear ImGui's Metal 4 backend records into the open encoder
    // but allocates its per-frame buffers and its UI pipeline off the command buffer's device.
    //
    // Both assert a pass is open, which is the only state in which either is meaningful to hand
    // out: the command buffer is a valid pointer for this list's whole lifetime, but Metal 4 only
    // accepts encoding into it between beginCommandBuffer and endCommandBuffer, and outside that
    // window the encoding aborts inside Metal with nothing pointing back at the RHI call.
    MTL4::CommandBuffer* commandBuffer() const;
    MTL4::RenderCommandEncoder* currentEncoder() const;

private:
    // Claims the frame's next timestamp pair for `label` and writes the opening one. Both writes
    // straddle the encoder rather than sitting inside it -- see the note in beginRenderPass.
    void beginTimedPass(std::string_view label);

    // Writes the closing timestamp of the pass beginTimedPass most recently opened.
    void endTimedPass();

    // Emits the barrier the textureBarrier/bufferBarrier calls since the last pass accumulated, as
    // the first command of the pass that just opened. `consumerStages` are the stages of the
    // opening encoder, which is the consumer side by construction. Clears the pending set, so a
    // second pass does not re-wait on a dependency already satisfied.
    void emitPendingBarrier(MTL4::CommandEncoder* encoder, MTL::Stages consumerStages);

    // True while any pass is open. Bindings and the frame's data arena are shared by the render
    // and compute pass kinds, so their scope check is "a pass is open" rather than a specific
    // encoder; the barriers, which are valid only *between* passes, check the same thing inverted,
    // and a copy pass counts for that.
    bool inPass() const { return inRenderPass() || inComputePass() || inCopyPass(); }

    // Bindings and frame data belong only to shader-bearing passes; a copy pass has no argument
    // table.
    bool inShaderPass() const { return inRenderPass() || inComputePass(); }

    std::vector<std::shared_ptr<Metal4TemporalScalerState>>* m_temporalScalers = nullptr;
    MTL::Fence* m_pendingTemporalFence = nullptr;
    MTL4::CommandBuffer* m_commandBuffer = nullptr;
    MTL4::ArgumentTable* m_argumentTable = nullptr;
    Metal4FrameArena* m_frameArena = nullptr;
    Metal4FrameDataTally* m_frameDataTally = nullptr;
    Metal4FrameTimestamps* m_timestamps = nullptr;
    NS::SharedPtr<MTL4::RenderCommandEncoder> m_encoder;
    NS::SharedPtr<MTL4::ComputeCommandEncoder> m_computeEncoder;
    // Metal 4 has no blit encoder: copies and fills are recorded on a compute encoder, so a copy
    // pass opens one of those and never binds a pipeline or the argument table to it.
    NS::SharedPtr<MTL4::ComputeCommandEncoder> m_copyEncoder;
    // The compute pipeline bound in the open compute pass, borrowed for its threadgroup shape --
    // Metal takes that shape at dispatch rather than at bind. Null outside a compute pass and
    // until the pass binds one; the pipeline itself is owned by the caller and outlives the frame.
    const Metal4ComputePipeline* m_computePipeline = nullptr;
    // The union of the queue stages the barriers recorded since the last pass produce from. Empty
    // means no barrier is pending; the next encoder to open emits one barrier for all of them.
    MTL::Stages m_pendingBarrierStages{};
    MTL4::VisibilityOptions m_pendingBarrierVisibility{};
};

} // namespace lmx::rhi::metal4
