//----------------------------------------------------------------------------------------------------------------------
/// @file Device.h
/// @brief Declares the device object handle, its creation and teardown, and its frame calls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>
#include <rojoRHI/BufferDesc.h>
#include <rojoRHI/Capabilities.h>
#include <rojoRHI/CommandList.h>
#include <rojoRHI/ComputePipelineDesc.h>
#include <rojoRHI/DeviceDesc.h>
#include <rojoRHI/FenceValue.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/FormatCapabilities.h>
#include <rojoRHI/GraphicsPipelineDesc.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/HeapDesc.h>
#include <rojoRHI/IndexRange.h>
#include <rojoRHI/Placement.h>
#include <rojoRHI/PlacementRequirements.h>
#include <rojoRHI/Queue.h>
#include <rojoRHI/QueueType.h>
#include <rojoRHI/Result.h>
#include <rojoRHI/SamplerDesc.h>
#include <rojoRHI/SamplerIndex.h>
#include <rojoRHI/Swapchain.h>
#include <rojoRHI/SwapchainDesc.h>
#include <rojoRHI/TextureData.h>
#include <rojoRHI/TextureDesc.h>
#include <rojoRHI/TextureViewDesc.h>
#include <rojoRHI/TileShape.h>
#include <rojoRHI/UpscalerDesc.h>
#include <rojoRHI/ViewIndex.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI {
    /// The owner of every resource, queue and frame slot RojoRHI creates. It wraps one backend pointer: copies refer to the
    /// same device, and a default-constructed one refers to none (VU-rojo-core-0004). It lives from `createDevice` to
    /// `destroyDevice`.
    class Device
    {
      public:
        /// Returns the device's capabilities; the reference stays valid until `destroyDevice`. Free-threaded and lock-free.
        const Capabilities& capabilities() const;

        /// Returns what the device supports for `format`: a table lookup, free-threaded and lock-free. The format is an
        /// enumerator (VU-rojo-format-0001) and is not `Format::Undefined` (VU-rojo-format-0005).
        FormatCapabilities formatCapabilities(Format format) const;

        /// Opens the next frame, first recycling the slot last opened `framesInFlight` frames earlier: its deferred
        /// destructions are released. No frame may be open (VU-rojo-device-0003). Thread class frame.
        void beginFrame();

        /// Closes the open frame (VU-rojo-device-0004); every command list acquired in it has been submitted
        /// (VU-rojo-device-0005). Thread class frame.
        void endFrame();

        /// Creates a buffer of `desc.size` bytes in `desc.memory` and labels it. `Upload` and `Readback` buffers are persistently
        /// mapped. Non-empty `initial` fills the first `initial.size()` bytes and the rest is undefined; it is at most `desc.size`
        /// (VU-rojo-buffer-0007) and empty for a `Sparse` or `Readback` buffer (VU-rojo-buffer-0008). The descriptor obeys
        /// VU-rojo-buffer-0003 to 0006. A non-null `placement.heap` creates the buffer inside that heap under VU-rojo-heap-0003
        /// to 0006. Out of memory is an error result. Free-threaded.
        Result<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial = {}, Placement placement = {});

        /// Creates a placement heap of at least `desc.size` bytes (VU-rojo-heap-0001), labels it and returns it. Out of memory
        /// is an error result. Free-threaded.
        Result<Heap> createHeap(const HeapDesc& desc);

        /// Returns the bytes a buffer described by `desc` occupies inside a heap and the alignment its offset must satisfy.
        /// The descriptor obeys its own rules and is not `Sparse` (VU-rojo-heap-0002). Allocates nothing and returns the same
        /// value for equal descriptors on one device. Free-threaded.
        PlacementRequirements placementRequirements(const BufferDesc& desc);

        /// Returns the bytes a texture described by `desc` occupies inside a heap and the alignment its offset must satisfy.
        /// The descriptor obeys the texture rules and is not `Sparse` (VU-rojo-heap-0002). Allocates nothing and returns the
        /// same value for equal descriptors on one device. Free-threaded.
        PlacementRequirements placementRequirements(const TextureDesc& desc);

        /// Creates a texture and labels it. `initial` is empty or holds one entry per subresource, index `mip + layer * mips`
        /// (VU-rojo-texture-0018), and is empty for a `Sparse`, non-`Sampled` or multisampled texture
        /// (VU-rojo-texture-0019 to 0021); each entry obeys VU-rojo-texture-0013 to 0015. The descriptor obeys
        /// VU-rojo-texture-0001 to 0012. A non-null `placement.heap` creates the texture inside that heap under
        /// VU-rojo-heap-0003 to 0006. Out of memory is an error result. Free-threaded.
        Result<Texture> createTexture(const TextureDesc& desc, std::span<const TextureData> initial = {}, Placement placement = {});

        /// Allocates a slot in the view heap, writes a view of `texture` into it and returns the slot's index. The descriptor
        /// obeys VU-rojo-texture-0016, 0017 and 0022 to 0027, and the texture is destroyed only after every slot holding a
        /// view of it has been destroyed (VU-rojo-texture-0028). A full view heap is an error result. Free-threaded.
        Result<ViewIndex> createView(Texture texture, const TextureViewDesc& desc);

        /// Allocates a slot in the sampler heap, writes a sampler into it and returns the slot's index. The descriptor obeys
        /// VU-rojo-texture-0029 and 0030. A full sampler heap is an error result. Free-threaded.
        Result<SamplerIndex> createSampler(const SamplerDesc& desc);

        /// Reserves `count` contiguous slots in the view heap (VU-rojo-texture-0032), returned with `Kind::Views`. The slots
        /// hold no view until `writeView` writes one. No contiguous run of `count` free slots is an error result.
        /// Free-threaded.
        Result<IndexRange> reserveViews(uint32_t count);

        /// Writes a view of `texture` into slot `range.base + index` of a view range, replacing any view there; the range
        /// is a Views range and `index` is inside it (VU-rojo-texture-0033), and `desc` and `texture` obey
        /// VU-rojo-texture-0034, which also reports a format the device does not support for `desc.usage` (`createView` reports
        /// that as VU-rojo-format-0003), and `range` is a reserved, unreleased range as returned (VU-rojo-texture-0031). The write takes effect for work
        /// submitted after it. Free-threaded; writes to distinct slots may run concurrently.
        void writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc);

        /// Reserves `count` contiguous slots in the sampler heap (VU-rojo-texture-0035), returned with `Kind::Samplers`. The
        /// slots hold no sampler until `writeSampler` writes one. No contiguous run of `count` free slots is an error
        /// result. Free-threaded.
        Result<IndexRange> reserveSamplers(uint32_t count);

        /// Writes a sampler into slot `range.base + index` of a sampler range, replacing any sampler there; the range is a
        /// Samplers range and `index` is inside it (VU-rojo-texture-0036), `desc` obeys VU-rojo-texture-0037 and `range` obeys
        /// VU-rojo-texture-0031. The
        /// write takes effect for work submitted after it. Free-threaded; writes to distinct slots may run concurrently.
        void writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc);

        /// Releases a caller-owned range, deferred like `destroy`: its slots and what they hold return to the heap once the
        /// last frame that could have read them has retired. The range obeys VU-rojo-texture-0038. Free-threaded.
        void release(IndexRange range);

        /// Returns the device's queue of `type`; the same queue on every call. The device owns it and it is never destroyed
        /// separately. Free-threaded.
        Queue queue(QueueType type);

        /// Returns a command list for `type` in the recording state from the open frame's pool, labelled `label` (empty selects
        /// a `rojorhi.`-prefixed default). A frame is open (VU-rojo-cmd-0001). The list belongs to that frame: it returns to the
        /// pool when the frame retires, and every list acquired in a frame is submitted before its `endFrame`
        /// (VU-rojo-device-0005). Free-threaded.
        CommandList acquireCommandList(QueueType type, std::string_view label);

        /// Loads a precompiled shader library from `blob`, which is not empty (VU-rojo-pipeline-0003), and labels it. A blob not
        /// built for the active backend or malformed is `ErrorCategory::DriverFailure`; one stamped with another
        /// `ROJORHI_SHADER_ABI_VERSION` is `ErrorCategory::ShaderAbiMismatch`. The bytes are consumed before return.
        /// Free-threaded.
        Result<ShaderLibrary> loadShaderLibrary(std::span<const std::byte> blob, std::string_view label);

        /// Creates a graphics pipeline synchronously; on return binding it never compiles. The descriptor obeys
        /// VU-rojo-pipeline-0001, 0002 and 0004 to 0012, and VU-rojo-format-0002 and 0003 for its formats. The device's
        /// pipeline cache is consulted first. A compiler or driver failure is an error result. Free-threaded.
        Result<Pipeline> createPipeline(const GraphicsPipelineDesc& desc);

        /// Creates a compute pipeline synchronously; the descriptor obeys VU-rojo-pipeline-0001, 0002 and 0013. Otherwise as
        /// the graphics overload. Free-threaded.
        Result<Pipeline> createPipeline(const ComputePipelineDesc& desc);

        /// Returns the current pipeline cache as a blob to pass back through `DeviceDesc::pipelineCache` at a later
        /// `createDevice`; it covers every pipeline created so far, and one created concurrently may be missing. Failing to
        /// serialize is an error result. Free-threaded.
        Result<std::vector<std::byte>> pipelineCacheData();

        /// Creates a timeline fence whose completed value starts at `initial` and labels it. Out of memory is an error result.
        /// Free-threaded.
        Result<Fence> createFence(uint64_t initial, std::string_view label);

        /// Creates a swapchain bound to `desc.nativeSurface` for the graphics queue, at the surface's current extent. The
        /// description obeys VU-rojo-present-0001 to 0005, and the surface is not that of another live swapchain of this device
        /// (VU-rojo-present-0006). A color space or present mode the surface or display cannot provide is an error result.
        /// Presentation setup: it may wait. Free-threaded.
        Result<Swapchain> createSwapchain(const SwapchainDesc& desc);

        /// Creates the upscaler implementation `desc.kind` selects, with its private history; `Auto` selects the first kind of
        /// `capabilities().upscalers`. The description obeys VU-rojo-upscale-0001 to 0003. A kind the device does not list, an
        /// extent ratio or format the implementation rejects, or a platform refusal is an error result, never a substitution.
        /// Free-threaded and synchronous.
        Result<Upscaler> createUpscaler(const UpscalerDesc& desc);

        /// Returns the greatest value `fence` has reached. Never blocks. Free-threaded.
        uint64_t completedValue(Fence fence);

        /// Blocks until `value.fence` reaches `value.value` or `timeoutNs` nanoseconds pass; returns true when the value was
        /// reached. A `timeoutNs` of `UINT64_MAX` requires the value to be at most the fence's initial value or the greatest
        /// value already submitted for signal (VU-rojo-queue-0008). Free-threaded.
        bool wait(FenceValue value, uint64_t timeoutNs);

        /// Returns the shader-visible address of byte `offset` of `buffer`: `offset` is less than the buffer's size
        /// (VU-rojo-buffer-0012), the buffer is not `Readback` (VU-rojo-buffer-0013), and the address lies less than
        /// `Limits::maxAddrOffset` bytes from the buffer's start (VU-rojo-buffer-0011). Free-threaded; the same buffer and
        /// offset give the same value for the buffer's lifetime.
        Addr address(Buffer buffer, uint64_t offset = 0);

        /// Returns the CPU pointer to byte 0 of a persistently mapped buffer, valid until the buffer is destroyed; the buffer is
        /// `Upload` or `Readback` (VU-rojo-buffer-0014). Free-threaded.
        std::byte* cpuPointer(Buffer buffer);

        /// Returns the tile shape of a sparse texture, fixed at creation. The texture was created with `TextureUsage::Sparse`
        /// (VU-rojo-sparse-0001) and is not a swapchain image (VU-rojo-present-0012). Records no GPU work. Free-threaded.
        TileShape tileShape(Texture texture);

        /// Returns the tile shape of a sparse buffer, fixed at creation. The buffer was created with `BufferUsage::Sparse`
        /// (VU-rojo-sparse-0001). Records no GPU work. Free-threaded.
        TileShape tileShape(Buffer buffer);

        /// Destroys a buffer, deferred: the handle is invalid at once and the slot is reused once the frame slot open at the
        /// call, or the next one opened if none is, retires. A sparse buffer's tiles are unmapped at the call. Free-threaded.
        void destroy(Buffer buffer);
        /// Destroys a texture, deferred as `destroy(Buffer)` is; a sparse texture's tiles are unmapped at the call.
        void destroy(Texture texture);
        /// Destroys a pipeline, deferred as `destroy(Buffer)` is.
        void destroy(Pipeline pipeline);
        /// Destroys a shader library, deferred as `destroy(Buffer)` is.
        void destroy(ShaderLibrary library);
        /// Destroys a heap, deferred as `destroy(Buffer)` is; the heap holds no placed resource and no mapped tile
        /// (VU-rojo-device-0008, VU-rojo-device-0009).
        void destroy(Heap heap);
        /// Destroys a fence, deferred as `destroy(Buffer)` is.
        void destroy(Fence fence);
        /// Destroys an upscaler, deferred as `destroy(Buffer)` is.
        void destroy(Upscaler upscaler);
        /// Releases a view slot into the deferred path; the index came from `createView` (VU-rojo-device-0010).
        void destroy(ViewIndex view);
        /// Releases a sampler slot into the deferred path; the index came from `createSampler` (VU-rojo-device-0010).
        void destroy(SamplerIndex sampler);
        /// Destroys a swapchain, deferred as `destroy(Buffer)` is; an image it holds acquired is released with it, and using the
        /// swapchain afterwards is undefined behaviour.
        void destroy(Swapchain swapchain);

      private:
        friend struct DeviceImpl;
        struct DeviceImpl* m_impl = nullptr;
    };

    /// Opens the system's GPU and creates a device. Returns `ErrorCategory::Unsupported` when `DeviceDesc::validation` is set
    /// in a Release build, or when the hardware floor is not met. Free-threaded; several devices may coexist.
    Result<Device> createDevice(const DeviceDesc& desc = {});

    /// Waits for all submitted work, releases every object the device owns and invalidates every handle it produced. No
    /// frame is open (VU-rojo-device-0002). Thread class exclusive.
    void destroyDevice(Device device);
} // namespace rojoRHI
