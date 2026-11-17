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
#include <string>
#include <string_view>

namespace rojoRHI {
    struct DeviceImpl;
} // namespace rojoRHI

namespace rojoRHI::metal4 {
    // Blocks until Metal ran the feedback handler and then released it, the last reference of the commit's completion
    // path, so a release by the caller never leaves the final one to Metal's notification thread, which holds no
    // autorelease pool. Counts nothing; the caller holds an autorelease pool.
    std::string commitAndWait(MTL4::CommandQueue* queue, std::span<const MTL4::CommandBuffer* const> buffers, std::string_view label);

    // A fourth queue, `rojorhi.upload`, that no `QueueType` names. A copy waits until Metal reports it complete and has
    // dropped every reference the commit took, so the data is visible to work any queue submits afterwards and every
    // object may be released between copies. Copies run one at a time under the mutex; the caller attaches the device
    // set to `queue()`.
    class Stager
    {
      public:
        // The caller holds an autorelease pool and calls `release` when this fails.
        Result<void> create(MTL::Device* device);

        // `bytes` is not empty. The device residency set is committed first when it is dirty; each residency commit
        // counts one `nativeHostCalls`.
        Result<void> uploadBuffer(DeviceImpl& device, std::span<const std::byte> bytes, MTL::Buffer* destination);

        // A dedicated buffer joins the stager's set for the copy; a placed one is resident through its heap. Tests read
        // Private memory through it.
        Result<void> copy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size);

        // One staging buffer holds every entry (index `mip + layer * mips`) at 256-byte aligned offsets; nothing
        // happens when every entry is empty.
        Result<void> uploadTexture(DeviceImpl& device, const core::TextureRecord& shape, std::span<const TextureData> initial, MTL::Texture* destination);

        // `destination` is a dedicated Shared buffer. Tests read Private textures through it.
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

        // Null before `create` and after `release`.
        MTL4::CommandQueue* queue() const;

        // Tolerates what `create` did not make. The caller holds an autorelease pool, has detached the device residency
        // set from the queue, and no copy is running.
        void release();

      private:
        // The caller holds `m_mutex` and an autorelease pool.
        Result<void> submitCopy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size);

        // The caller holds `m_mutex` and an autorelease pool.
        void commitSet(DeviceImpl& device);

        // The caller holds `m_mutex` and an autorelease pool.
        MTL4::ComputeCommandEncoder* begin(DeviceImpl& device);

        // A null `encoder` commits nothing and is a `DriverFailure`. The caller holds `m_mutex` and an autorelease
        // pool.
        Result<void> finish(DeviceImpl& device, MTL4::ComputeCommandEncoder* encoder);

        std::mutex m_mutex;
        MTL4::CommandQueue* m_queue = nullptr;
        MTL4::CommandAllocator* m_allocator = nullptr;
        MTL4::CommandBuffer* m_commands = nullptr;
        MTL::ResidencySet* m_set = nullptr;
    };
} // namespace rojoRHI::metal4
