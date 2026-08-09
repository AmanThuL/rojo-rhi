#pragma once
#include "RHI/Metal4/Metal4Common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lmx::rhi::metal4 {

// How many render passes of one frame carry timestamps. Sized like kUniformRingBytes -- a fixed
// per-frame budget that a real frame is expected to stay under, and whose exhaustion is a hard
// error rather than a silently dropped measurement.
inline constexpr uint32_t kMaxTimedPassesPerFrame = 16;

// Two timestamps per pass: one before its encoder opens, one after it closes.
inline constexpr uint32_t kTimestampsPerFrame = kMaxTimedPassesPerFrame * 2;

// One frame slot's GPU timestamps, owned by Metal4Device and written by the command list.
//
// The heap rotates on the same recycle discipline as the command allocators and uniform rings: the
// GPU writes into it for as long as the frame is in flight, so its entries may be read only after
// the frame-pacing event proves that frame retired, and are invalidated before the slot is handed
// to a new frame.
struct Metal4FrameTimestamps {
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
// It owns nothing that outlives a frame. The command buffer, the argument table and the uniform
// ring belong to the device (which outlives every command list it hands out), so all three are
// held as raw pointers; only the render command encoder -- created and destroyed inside a single
// beginRenderPass/endRenderPass pair -- is reference-counted here, because
// renderCommandEncoder() returns an autoreleased (+0) object that must survive the local
// autorelease pool it was created in.
//
// Those per-frame pointers are *not* fixed at construction: the device owns one argument table,
// one uniform ring and one timestamp slot per frame in flight and hands this list the current
// frame's set through resetForFrame, so none is ever written outside the frame that owns it.
// endFrameReset nulls them again at commit, which is what lets beginRenderPass's assert
// distinguish "no frame is open" from "a frame is open" at all -- without it a stale pointer would
// keep every check passing while the writes landed in a slot the GPU was still reading.
//
// Encoder-scoped calls (bindPipeline, bindBuffer, bindTexture, bindSampler, setUniforms, draw,
// drawIndexed) assert rather than return errors: calling them outside a pass is a sequencing
// bug, and the RHI CommandList methods return void.
// They also hold no autorelease pool of their own: none of them invokes an autoreleasing
// selector, so a per-call pool bought nothing and cost a create/drain on the hottest path in
// the backend. Verified against the vendored headers rather than assumed: bindPipeline, bindBuffer,
// draw are direct setters and draws on an already-retained encoder; setUniforms' whole call path
// (MTL::Buffer::contents(), length(), gpuAddress(), and MTL4::ArgumentTable::setAddress()) is
// scalar- and void-returning sendMessage; drawIndexed adds only MTL::Buffer::length() and
// gpuAddress() (scalar sends) plus drawIndexedPrimitives (a void send) on top of plain C++
// Metal4Buffer::handle() getter; bindTexture is MTL::Texture::usage() and gpuResourceID()
// (scalar sends, MTLTexture.hpp:293 and :241) into MTL4::ArgumentTable::setTexture() (a void
// send, MTL4ArgumentTable.hpp:80); bindSampler is MTL::SamplerState::gpuResourceID() (a scalar
// send, MTLSampler.hpp:337) into MTL4::ArgumentTable::setSamplerState() (a void send,
// MTL4ArgumentTable.hpp:179). So no path produces a +0 object.
// textureBarrier records a flag and touches Metal not at all; the barrier it defers
// is MTL4::CommandEncoder::barrierAfterQueueStages(), also a void send, encoded inside
// beginRenderPass's existing pool.
//
// Exactly one pool is load-bearing, in beginRenderPass: renderCommandEncoder() is the only
// selector here that returns +0. endRenderPass keeps a pool too (symmetry, and cheap insurance
// against a teardown path that starts autoreleasing) but it is *not* currently carrying
// anything -- endEncoding() is a void sendMessage and the encoder reset is a plain release.
class Metal4CommandList final : public CommandList {
public:
    explicit Metal4CommandList(MTL4::CommandBuffer* commandBuffer)
        : m_commandBuffer(commandBuffer) {}

    Metal4CommandList(const Metal4CommandList&) = delete;
    Metal4CommandList& operator=(const Metal4CommandList&) = delete;

    void beginRenderPass(const RenderPassDesc& desc) override;
    void bindPipeline(GraphicsPipeline& pipeline) override;
    void bindBuffer(uint32_t slot, Buffer& buffer) override;
    void bindTexture(uint32_t slot, Texture& texture) override;
    void bindSampler(uint32_t slot, Sampler& sampler) override;
    void setUniforms(uint32_t slot, const void* data, uint64_t size) override;
    void draw(uint32_t vertexCount, uint32_t firstVertex) override;
    void drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex) override;
    void endRenderPass() override;
    void textureBarrier(Texture& texture, TextureUse from, TextureUse to) override;

    // beginFrame's half of the per-frame rotation: point this command list at the frame's
    // argument table, uniform ring and timestamp slot. `uniformOffset` is the device's bump cursor
    // for that ring, already rewound to zero, and `timestamps` is the slot whose heap the device
    // has already invalidated and whose label array it has already cleared. Must be called before
    // any encoding in the frame; asserts no pass is open.
    void resetForFrame(MTL4::ArgumentTable* argumentTable, MTL::Buffer* uniformRing,
                       uint64_t* uniformOffset, Metal4FrameTimestamps* timestamps);

    // endFrame's half: forget the frame's table and ring once its work is committed, so that
    // encoding after endFrame fails our assert rather than quietly writing a slot the GPU owns.
    // Also where an unconsumed textureBarrier is caught -- see the .cpp.
    void endFrameReset();

    // True between beginRenderPass and endRenderPass. Metal4Device checks it so that ending a
    // frame with an open encoder is reported here rather than as a Metal abort at commit time.
    bool inRenderPass() const { return static_cast<bool>(m_encoder); }

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
    MTL4::CommandBuffer* m_commandBuffer = nullptr;
    MTL4::ArgumentTable* m_argumentTable = nullptr;
    MTL::Buffer* m_uniformRing = nullptr;
    uint64_t* m_uniformOffset = nullptr;
    Metal4FrameTimestamps* m_timestamps = nullptr;
    NS::SharedPtr<MTL4::RenderCommandEncoder> m_encoder;
    bool m_pendingBarrier = false;
};

} // namespace lmx::rhi::metal4
