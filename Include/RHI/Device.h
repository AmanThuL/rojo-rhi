//----------------------------------------------------------------------------------------------------------------------
/// @file Device.h
/// @brief Declares the RHI device, its creation surface, and per-pass GPU timings.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/ComputePipeline.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/Heap.h"
#include "RHI/Result.h"
#include "RHI/Sampler.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/Swapchain.h"
#include "RHI/Texture.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lmx::rhi {

/// How long the GPU spent on one pass of any kind, measured on the device timeline by timestamps
/// the backend writes at the pass boundaries -- callers record nothing.
///
/// label is the label the pass was begun with, with the backend's unnamed-pass fallback
/// substituted for an empty one. gpuMilliseconds covers the whole pass, load and store actions
/// included, and is wall time on the GPU rather than a sum of shader costs: a pass that overlaps
/// another still reports its own span, so times across a frame may add up to more than the frame
/// took.
/// Reports the GPU duration associated with one labeled pass.
struct PassTiming {
    std::string label;            ///< Render-pass diagnostic label.
    double gpuMilliseconds = 0.0; ///< Measured GPU wall time in milliseconds.
};

/// Selects optional behavior when creating an RHI device.
struct DeviceDesc {
    bool enableValidation = true; ///< Enables backend validation diagnostics.
};
/// Creates GPU resources and controls the three-frames-in-flight frame loop.
class Device {
public:
    /// Destroys the device and its backend state.
    virtual ~Device() = default;
    /// Creates a swapchain for a native presentation surface.
    virtual Result<std::unique_ptr<Swapchain>> createSwapchain(const SwapchainDesc&) = 0;
    /// Creates a GPU buffer and optionally uploads its initial contents.
    virtual Result<std::unique_ptr<Buffer>> createBuffer(const BufferDesc&,
                                                         const void* initialData) = 0;
    /// mips uploads initial content. Empty = no upload (a render target, or a texture a later pass
    /// fills). Otherwise mips.size() must be mipLevels * faceCount (6 for Cube, 1 for Tex2D),
    /// ordered mip-major per face: face0[mip0..N], face1[mip0..N], ... Anything else is a caller
    /// error and asserts.
    virtual Result<std::unique_ptr<Texture>>
    createTexture(const TextureDesc&, std::span<const TextureMip> mips = {}) = 0;

    /// Creates a placement heap for resources the caller positions itself. See Heap for the
    /// hazard-tracking contract the placed resources inherit.
    virtual Result<std::unique_ptr<Heap>> createHeap(const HeapDesc&) = 0;

    /// Creates a texture occupying `heap` from `offset`, which must be a multiple of the alignment
    /// textureSizeAlign(desc) reports and must leave that call's size within the heap. The texture
    /// has no initial contents -- a placed resource is device-private, so `desc` may not ask for
    /// cpuReadback and there is no upload span -- and it must not outlive the heap.
    virtual Result<std::unique_ptr<Texture>> createPlacedTexture(Heap&, uint64_t offset,
                                                                 const TextureDesc&) = 0;
    /// The buffer counterpart of createPlacedTexture, on the same terms and with bufferSizeAlign as
    /// the size and alignment source.
    virtual Result<std::unique_ptr<Buffer>> createPlacedBuffer(Heap&, uint64_t offset,
                                                               const BufferDesc&) = 0;

    /// Reports what `desc` costs inside a heap. Answered by the backend for that exact descriptor
    /// and stable for the device's lifetime, so a caller may plan a whole heap's layout from it
    /// before creating anything.
    virtual SizeAlign textureSizeAlign(const TextureDesc&) const = 0;
    /// The buffer counterpart of textureSizeAlign.
    virtual SizeAlign bufferSizeAlign(const BufferDesc&) const = 0;

    /// Creates an immutable sampler.
    virtual Result<std::unique_ptr<Sampler>> createSampler(const SamplerDesc&) = 0;
    /// pathNoExt: resolves "<pathNoExt>.metallib" (precompiled) first, else
    /// "<pathNoExt>.metal" (runtime-compiled MSL, Metal 4 language version).
    virtual Result<std::unique_ptr<ShaderLibrary>>
    /// Loads the shader library resolved from `pathNoExt`.
    loadShaderLibrary(std::string_view pathNoExt) = 0;
    /// Creates an immutable graphics pipeline.
    virtual Result<std::unique_ptr<GraphicsPipeline>>
    /// Creates a graphics pipeline from the supplied descriptor.
    createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
    /// Creates an immutable compute pipeline.
    virtual Result<std::unique_ptr<ComputePipeline>>
    /// Creates a compute pipeline from the supplied descriptor.
    createComputePipeline(const ComputePipelineDesc&) = 0;

    /// Frame loop: beginFrame blocks on pacing (3 in flight), returns the frame CommandList.
    /// endFrame commits; if presentTo != nullptr, presents its acquired texture.
    /// Begins a paced frame and returns its command list.
    virtual CommandList& beginFrame() = 0;
    /// Commits the frame and optionally presents through `presentTo`.
    virtual void endFrame(Swapchain* presentTo) = 0;
    /// Blocks until all work submitted to this device completes.
    virtual void waitIdle() = 0;

    /// Per-pass GPU times of one past frame, in the order that frame began its passes.
    ///
    /// The reported frame is the newest one the GPU had finished by the time of the most recent
    /// beginFrame(). That lag is not an implementation detail to be tuned away: a frame's
    /// timestamps are written by the GPU as it executes, so they are readable only once that frame
    /// retires, and this RHI resolves them at the one point retirement is already proven --
    /// beginFrame's pacing wait. Reading them any earlier would mean stalling the CPU on the GPU
    /// mid-frame.
    ///
    /// Consequences a caller can rely on:
    ///   - empty until a beginFrame() observes a retired frame, so the whole first frame reports
    ///     nothing;
    ///   - to obtain the timings of a *specific* frame, end it, waitIdle(), then call beginFrame()
    ///     once more -- that call publishes exactly that frame;
    ///   - in a continuous loop the readout trails the open frame by a few frames and never stalls.
    /// A frame with no passes reports an empty span, not the previous frame's numbers.
    ///
    /// The span is owned by the device and is invalidated by the next beginFrame().
    /// Returns the newest retired frame's pass timings until the next beginFrame call.
    virtual std::span<const PassTiming> passTimings() const = 0;

    /// Which frame passTimings() describes, so a caller can attribute a measurement to the frame
    /// it came from rather than to "some frame a few back". Frames are numbered in beginFrame()
    /// order starting at one, so a caller that counts its own beginFrame() calls shares the
    /// numbering; zero means nothing has been published yet.
    /// Returns the frame number the current pass timings were measured on.
    virtual uint64_t passTimingsFrame() const = 0;

    /// The newest frame beginFrame() has opened, in the same numbering passTimingsFrame() reports:
    /// frames count from one in beginFrame() order, and zero means beginFrame() has not been called
    /// yet. It names the frame being *built* where passTimingsFrame() names the frame most recently
    /// *measured*, so the two are equal only in the degenerate case of a drained device, and a
    /// caller joining CPU-side per-frame state to retired GPU timings compares one against the
    /// other rather than assuming they agree.
    ///
    /// The value does not change while a frame is open, so everything recorded between beginFrame()
    /// and endFrame() belongs to the number this reports.
    /// Returns the number of the newest frame beginFrame has opened.
    virtual uint64_t frameNumber() const = 0;

    /// Returns the backend device's human-readable name.
    virtual std::string_view deviceName() const = 0;
};

/// Creates the platform RHI device.
Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc = {});

} // namespace lmx::rhi
