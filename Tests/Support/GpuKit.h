//----------------------------------------------------------------------------------------------------------------------
/// @file GpuKit.h
/// @brief Declares the helper that runs recorded lists on a GPU and reads buffers and textures back for the conformance cases.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    /// A device with an open frame for the conformance cases, built on public entries only. It acquires lists, runs them to
    /// completion on the GPU and reads buffers and textures back through copies into Readback buffers. Every run waits on the
    /// host, so work of one run is complete before the next begins and no case needs a barrier between runs.
    class GpuKit
    {
      public:
        /// Creates the device with `framesInFlight` frame slots, and with `DeviceDesc::scopeTimings` when `scopeTimings` is
        /// set, and opens a frame. `validation` selects the tracker, which only Debug builds support.
        explicit GpuKit(uint32_t framesInFlight = 2, bool scopeTimings = false, bool validation = false);

        /// Ends the frame and destroys the device.
        ~GpuKit();

        GpuKit(const GpuKit&) = delete;
        GpuKit& operator=(const GpuKit&) = delete;

        /// Acquires a list of `type` labelled `label` in the open frame.
        CommandList list(QueueType type, std::string_view label);

        /// Ends `lists`, which this kit acquired, and submits them in order, each run of consecutive lists of one type in one
        /// submission to the queue of that type. The first submission waits on `wait`; each later one waits on the value the one
        /// before it signals on the kit's fence. When a compute or copy queue took part, a graphics submission with no lists
        /// waits on the last value, joining the work into the frame's graphics queue (VU-rojo-device-0006). It then waits on the
        /// host for the last value.
        void run(std::span<const CommandList> lists, std::span<const FenceValue> wait = {});

        /// Ends the open frame and opens the next. Every list acquired in the frame has been run.
        void nextFrame();

        /// Creates a buffer of `size` bytes in `memory` with `usage`, holding `initial` from its start when it is not empty, at
        /// `placement` (a dedicated allocation by default).
        Buffer buffer(uint64_t size, Memory memory, BufferUsage usage = BufferUsage::None, std::span<const std::byte> initial = {}, Placement placement = {});

        /// Returns every byte of `readback`, a `Memory::Readback` buffer this kit created, through its CPU pointer.
        std::vector<std::byte> read(Buffer readback);

        /// Returns every byte of `source`, a buffer this kit created, copied into a new Readback buffer by a list of `type`.
        std::vector<std::byte> readBuffer(Buffer source, QueueType type = QueueType::Graphics);

        /// Returns the texels of `region` of `texture`, whose format is `format`, copied into a new Readback buffer with each row
        /// 256-byte aligned, in tight rows of texels or blocks: the rows of each depth slice, the slices of each layer, then the
        /// layers.
        std::vector<std::byte> readTexture(Texture texture, const TextureRegion& region, Format format);

        Device device; ///< The device; a frame is open from construction to destruction.

      private:
        /// A list this kit acquired, with the queue type it was acquired for.
        struct Acquired
        {
            CommandList list; ///< The list.
            QueueType type;   ///< Its queue type.
        };

        /// A buffer this kit created, with its size.
        struct Sized
        {
            Buffer buffer; ///< The buffer.
            uint64_t size; ///< Its size in bytes.
        };

        /// Returns the queue type `list` was acquired for; fails the case when this kit did not acquire it.
        QueueType typeOf(CommandList list) const;

        /// Returns the size of `buffer`; fails the case when this kit did not create it.
        uint64_t sizeOf(Buffer buffer) const;

        Fence m_fence;                 ///< The fence each submission of `run` signals.
        uint64_t m_value = 0;          ///< The last value submitted for signal on `m_fence`.
        std::vector<Acquired> m_lists; ///< The lists acquired in the open frame.
        std::vector<Sized> m_buffers;  ///< The buffers created, with their sizes.
    };

    /// Returns `count` bytes whose values start at `seed` and step by a prime, so neighbouring bytes and distinct seeds differ
    /// and none is zero.
    std::vector<std::byte> pattern(size_t count, uint32_t seed);
} // namespace rojoRHI::test
