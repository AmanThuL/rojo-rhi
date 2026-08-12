//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Device.h
/// @brief Declares the Metal 4 device and its frame-in-flight backend state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "Metal4CommandList.h"
#include "Metal4Common.h"
#include "Metal4FrameArena.h"
#include "RHI/Device.h"
#include "RHI/Metal4/Metal4FrameData.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lmx::rhi::metal4 {

inline constexpr uint32_t kFramesInFlight = 3;

static_assert(kFrameDataSlotCount == kFramesInFlight,
              "the frame-data arena is per frame-in-flight slot, so the count the extension header "
              "publishes must be the backend's own");

// Ownership convention for the whole Metal 4 backend: every metal-cpp object this class
// owns is held in an NS::SharedPtr obtained with NS::TransferPtr, because all the
// factories used here are `new*` methods that hand back a +1 reference. Members are
// therefore released in reverse declaration order -- which is reverse creation order --
// without a hand-written release sequence that could drift out of sync. The destructor
// body only performs the *unwiring* that has to happen before any release (detaching the
// residency set from the queue).
class Metal4Device final : public Device {
public:
    static Result<std::unique_ptr<Device>> create(const DeviceDesc& desc);

    ~Metal4Device() override;

    Metal4Device(const Metal4Device&) = delete;
    Metal4Device& operator=(const Metal4Device&) = delete;

    Result<std::unique_ptr<Swapchain>> createSwapchain(const SwapchainDesc& desc) override;
    Result<std::unique_ptr<Buffer>> createBuffer(const BufferDesc& desc,
                                                 const void* initialData) override;
    Result<std::unique_ptr<Texture>> createTexture(const TextureDesc& desc,
                                                   std::span<const TextureMip> mips) override;
    Result<std::unique_ptr<Heap>> createHeap(const HeapDesc& desc) override;
    Result<std::unique_ptr<Texture>> createPlacedTexture(Heap& heap, uint64_t offset,
                                                         const TextureDesc& desc) override;
    Result<std::unique_ptr<Buffer>> createPlacedBuffer(Heap& heap, uint64_t offset,
                                                       const BufferDesc& desc) override;
    SizeAlign textureSizeAlign(const TextureDesc& desc) const override;
    SizeAlign bufferSizeAlign(const BufferDesc& desc) const override;
    Result<std::unique_ptr<Sampler>> createSampler(const SamplerDesc& desc) override;
    Result<std::unique_ptr<ShaderLibrary>> loadShaderLibrary(std::string_view pathNoExt) override;
    Result<std::unique_ptr<GraphicsPipeline>>
    createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    Result<std::unique_ptr<ComputePipeline>>
    createComputePipeline(const ComputePipelineDesc& desc) override;

    CommandList& beginFrame() override;
    void endFrame(Swapchain* presentTo) override;
    void waitIdle() override;

    std::span<const PassTiming> passTimings() const override { return m_passTimings; }

    uint64_t passTimingsFrame() const override { return m_resolvedFrame; }

    uint64_t frameNumber() const override { return m_frameNumber; }

    std::string_view deviceName() const override { return m_deviceName; }

    // Backend-internal, same role as the handle() on every resource wrapper: sibling Metal 4
    // files reach the native object through it (Metal4Capture needs the MTLDevice to name as a
    // capture object). Deliberately not on the RHI Device interface -- no Metal type appears in
    // RHI.h.
    MTL::Device* handle() const { return m_device.get(); }

    // Same standing as handle(): backend-internal, deliberately absent from the RHI Device
    // interface. Metal4ImGui hands this queue to Dear ImGui's Metal 4 backend, which attaches its
    // own residency set to it -- so ImGui's font atlas and vertex buffers are resident for the
    // command buffers this device commits, without either side knowing about the other's set.
    MTL4::CommandQueue* queue() const { return m_queue.get(); }

    // Backing implementation of metal4::frameDataCounters(). Backend-internal on the same terms as
    // handle() and queue(): the counters are test and capture evidence, and putting them on the
    // RHI Device interface would make them a permanent performance-query API instead.
    FrameDataCounters frameDataCounters() const;

    // The slot of the per-frame rotation (command allocator, argument table, frame-data arena)
    // belonging to the frame currently being built: the open frame while one is open, and the
    // frame the next beginFrame() will open while none is.
    //
    // The two cases are not cosmetic. beginFrame() increments m_frameNumber *before* deriving its
    // slot, so `m_frameNumber % kFramesInFlight` names the open frame only while a frame is open
    // -- outside one it still names the frame that just ended, which is the frame whose GPU work
    // this device is now waiting on. Metal4ImGui::imguiNewFrame() runs in exactly that gap (Dear
    // ImGui requires its renderer's NewFrame before ImGui::NewFrame(), which is before any of the
    // UI-building code that decides what the frame renders), and the slot it passes on has to be
    // the one whose GPU work will read the buffers ImGui is about to fill -- the frame about to
    // open. Hence the +1 rather than a second accessor the caller could pick wrongly.
    uint32_t frameInFlightIndex() const {
        return static_cast<uint32_t>((m_frameOpen ? m_frameNumber : m_frameNumber + 1) %
                                     kFramesInFlight);
    }

private:
    Metal4Device() = default;

    // Republishes m_passTimings from the newest frame slot the pacing event proves retired and
    // that has not been published already. Called by beginFrame after its wait, so it never blocks
    // on the GPU and never reads a heap the GPU may still be writing.
    void resolveRetiredPassTimings();

    // Converts one pass's raw timestamp pair into milliseconds; asserts the pair is a resolved,
    // ordered measurement rather than a counter Metal declined to write.
    double passMilliseconds(uint64_t beginTicks, uint64_t endTicks) const;

    // Owns the characters the deviceName() view points at -- MTL::Device::name()'s
    // utf8String() buffer is only valid while the autorelease pool that produced it lives.
    std::string m_deviceName;

    // Declaration order == creation order; see the ownership note above.
    NS::SharedPtr<MTL::Device> m_device;
    NS::SharedPtr<MTL4::CommandQueue> m_queue;
    NS::SharedPtr<MTL4::Compiler> m_compiler;
    NS::SharedPtr<MTL::ResidencySet> m_residency;
    std::array<NS::SharedPtr<MTL4::CommandAllocator>, kFramesInFlight> m_allocators;
    NS::SharedPtr<MTL::SharedEvent> m_frameEvent;
    // One command buffer for the whole device, re-opened against this frame's allocator by
    // every beginFrame. In Metal 4 the recorded commands live in the *allocator*, not in the
    // command buffer, so the buffer is a reusable encoding handle and the ring of allocators
    // (plus the shared-event pacing) is what keeps a frame from overwriting in-flight work.
    NS::SharedPtr<MTL4::CommandBuffer> m_commandBuffer;
    std::array<NS::SharedPtr<MTL4::ArgumentTable>, kFramesInFlight> m_argumentTables;
    // The per-frame data arenas, on the same rotation as the allocators and argument tables and
    // for the same reason: a slot's pages hold the blocks the GPU reads for as long as that frame
    // is in flight, so only the beginFrame wait proves the slot is reusable.
    std::array<Metal4FrameArena, kFramesInFlight> m_frameArenas;
    // What the frame that last owned an arena slot left in it. Pure bookkeeping: nothing reads it
    // to *decide* anything, and removing it would change no rendering. It turns the recycle assert
    // in beginFrame from a claim about pacing into a check against the shared event, and names
    // what would have been overwritten.
    struct FrameArenaUse {
        uint64_t frameNumber = 0;
        uint64_t bytesUsed = 0;
        uint32_t pagesUsed = 0;
    };
    std::array<FrameArenaUse, kFramesInFlight> m_frameArenaUse{};
    Metal4FrameDataTally m_frameDataTally;
    // GPU pass timestamps, on the same rotation and for the same reason as the arenas above: the
    // GPU writes a frame's entries while that frame is in flight.
    std::array<Metal4FrameTimestamps, kFramesInFlight> m_frameTimestamps;
    // Not a member by value: Metal4CommandList's constructor needs the command buffer above,
    // which does not exist until create() has run.
    std::optional<Metal4CommandList> m_commandList;

    // What passTimings() hands out; see the contract on the RHI declaration. Rebuilt wholesale by
    // resolveRetiredPassTimings, which is why the span is documented as valid only until the next
    // beginFrame.
    std::vector<PassTiming> m_passTimings;
    // The frame m_passTimings describes, so a slot is never published twice and an older slot can
    // never overwrite a newer publication.
    uint64_t m_resolvedFrame = 0;
    // GPU timestamp ticks per second, queried once from the device. Fixed at creation because the
    // conversion must be identical for every frame the readout compares.
    uint64_t m_timestampTicksPerSecond = 0;

    uint64_t m_frameNumber = 0;
    // Guards the beginFrame/endFrame pairing; see the assertions in both.
    bool m_frameOpen = false;
};

} // namespace lmx::rhi::metal4
