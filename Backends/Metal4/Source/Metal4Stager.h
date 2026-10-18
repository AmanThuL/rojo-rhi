//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Stager.h
/// @brief Declares the stager that copies the initial data of Private resources on a device's internal upload queue.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace rojoRHI {
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    /// The device's internal upload path: a fourth queue, `rojorhi.upload`, that no `QueueType` names, with one allocator, one
    /// command buffer and a residency set of its own for the buffers a copy makes. A copy commits on the upload queue and
    /// waits until Metal reports it complete and has dropped every reference the commit took, so the data is visible to work
    /// any queue submits afterwards and every object of the stager may be released at any time between copies. Copies run
    /// one at a time under the stager's mutex; the caller attaches the device residency set to `queue()`.
    class Stager
    {
      public:
        /// Creates the upload queue, the allocator, the command buffer and the staging residency set on `device`. The caller
        /// holds an autorelease pool and calls `release` when this fails.
        Result<void> create(MTL::Device* device);

        /// Copies `bytes`, which are not empty, into the first bytes of `destination` through a Shared staging buffer and waits
        /// for the copy. It commits the device residency set of `device` first when it is dirty, and the stager's own set as
        /// the staging buffer joins and leaves it, counting each commit as one `nativeHostCalls`; it counts the staging buffer
        /// as one `nativeCreateCalls` and the submission as one `nativeSubmitCalls`. A Metal failure, at creation or in
        /// execution, is a `DriverFailure` naming the staging buffer.
        Result<void> uploadBuffer(DeviceImpl& device, std::span<const std::byte> bytes, MTL::Buffer* destination);

        /// Copies the first `size` bytes of `source` into `destination` and waits, as `uploadBuffer` does, counting every
        /// residency commit and the submission. A dedicated buffer joins the stager's set for the copy; a placed one is
        /// resident through its heap. Tests read Private memory through it.
        Result<void> copy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size);

        /// Copies every non-empty entry of `initial`, one per subresource of a texture shaped as `shape` (index `mip + layer *
        /// mips`), into `destination` and waits, as `uploadBuffer` does: one staging buffer holds them all at 256-byte aligned
        /// offsets, and one encoder copies each into its mip and slice, a face counting as a slice. It counts and fails as
        /// `uploadBuffer` does, and does nothing when every entry is empty.
        Result<void> uploadTexture(DeviceImpl& device, const core::TextureRecord& shape, std::span<const TextureData> initial, MTL::Texture* destination);

        /// Copies `size` texels of mip `level` of slice `slice` of `source` into `destination`, a dedicated Shared buffer, in
        /// rows of `bytesPerRow` and slices of `bytesPerImage` bytes, and waits, counting as `copy` does. Tests read Private
        /// textures through it.
        Result<void> copyTexture(
            DeviceImpl& device,
            const MTL::Texture* source,
            uint32_t slice,
            uint32_t level,
            MTL::Size size,
            MTL::Buffer* destination,
            uint64_t bytesPerRow,
            uint64_t bytesPerImage
        );

        /// Returns the upload queue; null before `create` and after `release`.
        MTL4::CommandQueue* queue() const;

        /// Releases what `create` made, in reverse order, tolerating what it did not make. The caller holds an autorelease
        /// pool, has detached the device residency set from the queue, and no copy is running.
        void release();

      private:
        /// Records and commits one copy of `size` bytes from `source` to `destination`, whose allocations are resident, counts
        /// the submission and waits as the class describes. The caller holds `m_mutex` and an autorelease pool.
        Result<void> submitCopy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size);

        /// Commits the stager's set and counts one `nativeHostCalls`. The caller holds `m_mutex` and an autorelease pool.
        void commitSet(DeviceImpl& device);

        /// Commits the device set when it is dirty, counting one `nativeHostCalls`, begins the command buffer with the
        /// stager's set and returns its compute encoder, null when Metal made none. The caller holds `m_mutex` and an
        /// autorelease pool.
        MTL4::ComputeCommandEncoder* begin(DeviceImpl& device);

        /// Ends `encoder` and the command buffer, commits it, counts the submission and waits as the class describes. A null
        /// `encoder` commits nothing and is a `DriverFailure`. The caller holds `m_mutex` and an autorelease pool.
        Result<void> finish(DeviceImpl& device, MTL4::ComputeCommandEncoder* encoder);

        std::mutex m_mutex;
        MTL4::CommandQueue* m_queue = nullptr;
        MTL4::CommandAllocator* m_allocator = nullptr;
        MTL4::CommandBuffer* m_commands = nullptr;
        MTL::ResidencySet* m_set = nullptr;
    };
} // namespace rojoRHI::metal4
