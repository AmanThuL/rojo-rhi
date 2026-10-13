//----------------------------------------------------------------------------------------------------------------------
/// @file Device.h
/// @brief Declares the device object handle, its creation and teardown, and its frame calls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>
#include <rojoRHI/BufferDesc.h>
#include <rojoRHI/Capabilities.h>
#include <rojoRHI/DeviceDesc.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/FormatCapabilities.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/HeapDesc.h>
#include <rojoRHI/Placement.h>
#include <rojoRHI/PlacementRequirements.h>
#include <rojoRHI/Result.h>
#include <rojoRHI/SamplerIndex.h>
#include <rojoRHI/Swapchain.h>
#include <rojoRHI/ViewIndex.h>

#include <cstddef>
#include <cstdint>
#include <span>

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

        /// Closes the open frame (VU-rojo-device-0004). Thread class frame.
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

        /// Returns the shader-visible address of byte `offset` of `buffer`: `offset` is less than the buffer's size
        /// (VU-rojo-buffer-0012), the buffer is not `Readback` (VU-rojo-buffer-0013), and the address lies less than
        /// `Limits::maxAddrOffset` bytes from the buffer's start (VU-rojo-buffer-0011). Free-threaded; the same buffer and
        /// offset give the same value for the buffer's lifetime.
        Addr address(Buffer buffer, uint64_t offset = 0);

        /// Returns the CPU pointer to byte 0 of a persistently mapped buffer, valid until the buffer is destroyed; the buffer is
        /// `Upload` or `Readback` (VU-rojo-buffer-0014). Free-threaded.
        std::byte* cpuPointer(Buffer buffer);

        /// Destroys a buffer, deferred: the handle is invalid at once and the slot is reused once the frame slot open at the
        /// call, or the next one opened if none is, retires. Free-threaded.
        void destroy(Buffer buffer);
        /// Destroys a texture, deferred as `destroy(Buffer)` is.
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
        /// Destroys a swapchain; using it afterwards is undefined behaviour.
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
