//----------------------------------------------------------------------------------------------------------------------
/// @file Device.h
/// @brief Declares the device object handle, its creation and teardown, and its frame calls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Capabilities.h>
#include <rojoRHI/DeviceDesc.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/FormatCapabilities.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Result.h>
#include <rojoRHI/SamplerIndex.h>
#include <rojoRHI/Swapchain.h>
#include <rojoRHI/ViewIndex.h>

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
