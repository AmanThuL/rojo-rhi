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
    // Built on public entries only. Every run waits on the host, so work of one run is complete before the next begins and
    // no case needs a barrier between runs.
    class GpuKit
    {
      public:
        // `validation` selects the tracker, which only Debug builds support.
        explicit GpuKit(uint32_t framesInFlight = 2, bool scopeTimings = false, bool validation = false, bool debugMarkers = false);

        ~GpuKit();

        GpuKit(const GpuKit&) = delete;
        GpuKit& operator=(const GpuKit&) = delete;

        CommandList list(QueueType type, std::string_view label);

        // Each run of consecutive lists of one type is one submission to the queue of that type. The first submission waits on
        // `wait`; each later one waits on the value the one before it signals on the kit's fence. When a compute or copy queue
        // took part, a graphics submission with no lists waits on the last value, joining the work into the frame's graphics
        // queue (VU-rojo-device-0006). It then waits on the host for the last value.
        void run(std::span<const CommandList> lists, std::span<const FenceValue> wait = {});

        // Every list acquired in the frame has been run.
        void nextFrame();

        Buffer buffer(uint64_t size, Memory memory, BufferUsage usage = BufferUsage::None, std::span<const std::byte> initial = {}, Placement placement = {});

        // `readback` is a `Memory::Readback` buffer this kit created; it is read through its CPU pointer.
        std::vector<std::byte> read(Buffer readback);

        // `source` is a buffer this kit created.
        std::vector<std::byte> readBuffer(Buffer source, QueueType type = QueueType::Graphics);

        // The texels come back in tight rows of texels or blocks: the rows of each depth slice, the slices of each layer, then
        // the layers.
        std::vector<std::byte> readTexture(Texture texture, const TextureRegion& region, Format format);

        Device device; // The device; a frame is open from construction to destruction.

      private:
        struct Acquired
        {
            CommandList list;
            QueueType type;
        };

        struct Sized
        {
            Buffer buffer;
            uint64_t size;
        };

        // Fails the case when this kit did not acquire `list`.
        QueueType typeOf(CommandList list) const;

        // Fails the case when this kit did not create `buffer`.
        uint64_t sizeOf(Buffer buffer) const;

        Fence m_fence;
        uint64_t m_value = 0;
        std::vector<Acquired> m_lists;
        std::vector<Sized> m_buffers;
    };

    // Values start at `seed` and step by a prime, so neighbouring bytes and distinct seeds differ and none is zero.
    std::vector<std::byte> pattern(size_t count, uint32_t seed);
} // namespace rojoRHI::test
