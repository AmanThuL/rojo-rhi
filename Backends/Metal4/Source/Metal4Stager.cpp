//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Stager.cpp
/// @brief Implements the stager that copies the initial data of Private resources on a device's internal upload queue.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Stager.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/Formats.h"
#include "Core/Records.h"
#include "Core/Textures.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Metal/Metal.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rojoRHI::metal4 {
    namespace {
        /// The label of every staging buffer.
        constexpr std::string_view kStagingLabel = "rojorhi.staging";

        /// How long one slice of a wait for a copy lasts before it logs a warning.
        constexpr std::chrono::seconds kWaitSlice{ 10 };

        /// What the feedback handler of one commit reports back to the thread that waits for it.
        struct Completion
        {
            std::mutex mutex;                 ///< Guards the members below.
            std::condition_variable reported; ///< Notified when `finished` turns true.
            bool finished = false;            ///< True once Metal ran the feedback handler.
            std::string error;                ///< The execution error Metal reported; empty when there was none.
        };

        //==============================================================================================================
        /// Returns the `DriverFailure` of a Metal object `what` that was not created, with `error`'s description.
        Error notCreated(std::string_view what, NS::Error* error = nullptr)
        {
            return Error{ ErrorCategory::DriverFailure, std::format("{} was not created: {}", what, describe(error)) };
        }

        //==============================================================================================================
        /// Waits up to `slice` for every reference to `completion` but the caller's to be dropped, yielding at first and then
        /// sleeping between checks; returns true once the caller holds the last one.
        bool releasedWithin(const std::shared_ptr<Completion>& completion, std::chrono::steady_clock::duration slice)
        {
            // The release follows the handler's return closely, so the first checks only yield.
            constexpr std::chrono::microseconds kYieldFor{ 200 };
            constexpr std::chrono::microseconds kSleep{ 100 };
            const auto start = std::chrono::steady_clock::now();
            while (completion.use_count() > 1)
            {
                const auto waited = std::chrono::steady_clock::now() - start;
                if (waited >= slice)
                {
                    return false;
                }
                if (waited < kYieldFor)
                {
                    std::this_thread::yield();
                }
                else
                {
                    std::this_thread::sleep_for(kSleep);
                }
            }
            return true;
        }

        //==============================================================================================================
        /// Blocks until Metal ran the feedback handler that shares `completion` and then released the handler, on queue
        /// `label`. Logs a warning after each slice of `kWaitSlice` that passes. Returns the reported error.
        std::string waitForCompletion(std::shared_ptr<Completion>& completion, std::string_view label)
        {
            uint64_t waitedSeconds = 0;
            {
                std::unique_lock lock(completion->mutex);
                while (!completion->reported.wait_for(lock, kWaitSlice, [&] { return completion->finished; }))
                {
                    waitedSeconds += kWaitSlice.count();
                    ROJORHI_LOG_WARN("still waiting after {} s for a commit on queue \"{}\" to complete", waitedSeconds, label);
                }
            }
            // Metal releases the handler just after it returns, on its own thread.
            waitedSeconds = 0;
            while (!releasedWithin(completion, kWaitSlice))
            {
                waitedSeconds += kWaitSlice.count();
                ROJORHI_LOG_WARN("still waiting after {} s for queue \"{}\" to release a completed commit", waitedSeconds, label);
            }
            return std::move(completion->error);
        }
    } // namespace

    //==================================================================================================================
    std::string commitAndWait(MTL4::CommandQueue* queue, std::span<const MTL4::CommandBuffer* const> buffers, std::string_view label)
    {
        auto completion = std::make_shared<Completion>();
        MTL4::CommitOptions* options = MTL4::CommitOptions::alloc()->init();
        options->addFeedbackHandler([completion](MTL4::CommitFeedback* feedback) {
            // Metal's notification thread holds no autorelease pool, and the error's description is autoreleased.
            const AutoreleasePool handlerPool;
            std::string error = feedback->error() != nullptr ? describe(feedback->error()) : std::string();
            std::lock_guard lock(completion->mutex);
            completion->error = std::move(error);
            completion->finished = true;
            completion->reported.notify_all();
        });
        queue->commit(buffers.data(), buffers.size(), options);
        options->release();
        return waitForCompletion(completion, label);
    }

    //==================================================================================================================
    Result<void> Stager::create(MTL::Device* device)
    {
        MTL4::CommandQueueDescriptor* descriptor = MTL4::CommandQueueDescriptor::alloc()->init();
        descriptor->setLabel(makeString("rojorhi.upload"));
        NS::Error* error = nullptr;
        m_queue = device->newMTL4CommandQueue(descriptor, &error);
        descriptor->release();
        if (m_queue == nullptr)
        {
            return std::unexpected(notCreated("the queue rojorhi.upload", error));
        }
        m_allocator = device->newCommandAllocator();
        if (m_allocator == nullptr)
        {
            return std::unexpected(notCreated("the command allocator of rojorhi.upload"));
        }
        m_commands = device->newCommandBuffer();
        if (m_commands == nullptr)
        {
            return std::unexpected(notCreated("the command buffer of rojorhi.upload"));
        }
        MTL::ResidencySetDescriptor* setDescriptor = MTL::ResidencySetDescriptor::alloc()->init();
        setDescriptor->setLabel(makeString(kStagingLabel));
        error = nullptr;
        m_set = device->newResidencySet(setDescriptor, &error);
        setDescriptor->release();
        if (m_set == nullptr)
        {
            return std::unexpected(notCreated("the residency set of rojorhi.upload", error));
        }
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::uploadBuffer(DeviceImpl& device, std::span<const std::byte> bytes, MTL::Buffer* destination)
    {
        std::lock_guard lock(m_mutex);
        const AutoreleasePool pool;
        MTL::Buffer* staging = device.device->newBuffer(bytes.data(), bytes.size(), MTL::ResourceStorageModeShared);
        if (staging == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the staging buffer {} of {} bytes was not created", kStagingLabel, bytes.size()) }
            );
        }
        staging->setLabel(makeString(kStagingLabel));
        core::DeviceCounts::add(device.core.counts().nativeCreateCalls);
        m_set->addAllocation(staging);
        commitSet(device);
        Result<void> copied = submitCopy(device, staging, destination, bytes.size());
        m_set->removeAllocation(staging);
        commitSet(device);
        staging->release();
        if (!copied)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the copy from {} failed: {}", kStagingLabel, copied.error().message) });
        }
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::copy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size)
    {
        std::lock_guard lock(m_mutex);
        const AutoreleasePool pool;
        // A placed buffer is resident through its heap in the device set; a dedicated one joins the stager's set for the copy.
        const bool sourceJoins = source->heap() == nullptr;
        const bool destinationJoins = destination->heap() == nullptr;
        if (sourceJoins)
        {
            m_set->addAllocation(source);
        }
        if (destinationJoins)
        {
            m_set->addAllocation(destination);
        }
        commitSet(device);
        Result<void> copied = submitCopy(device, source, destination, size);
        if (destinationJoins)
        {
            m_set->removeAllocation(destination);
        }
        if (sourceJoins)
        {
            m_set->removeAllocation(source);
        }
        commitSet(device);
        return copied;
    }

    //==================================================================================================================
    Result<void> Stager::uploadTexture(DeviceImpl& device, const core::TextureRecord& shape, std::span<const TextureData> initial, MTL::Texture* destination)
    {
        // Each subresource starts at a 256-byte boundary, a multiple of every texel and block size.
        constexpr uint64_t kAlignment = 256;
        std::vector<uint64_t> offsets(initial.size());
        uint64_t size = 0;
        for (size_t index = 0; index < initial.size(); ++index)
        {
            if (!initial[index].bytes.empty())
            {
                offsets[index] = (size + kAlignment - 1) / kAlignment * kAlignment;
                size = offsets[index] + initial[index].bytes.size();
            }
        }
        if (size == 0)
        {
            return {};
        }
        std::lock_guard lock(m_mutex);
        const AutoreleasePool pool;
        MTL::Buffer* staging = device.device->newBuffer(size, MTL::ResourceStorageModeShared);
        if (staging == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the staging buffer {} of {} bytes was not created", kStagingLabel, size) }
            );
        }
        staging->setLabel(makeString(kStagingLabel));
        core::DeviceCounts::add(device.core.counts().nativeCreateCalls);
        auto* contents = static_cast<std::byte*>(staging->contents());
        for (size_t index = 0; index < initial.size(); ++index)
        {
            if (!initial[index].bytes.empty())
            {
                std::memcpy(contents + offsets[index], initial[index].bytes.data(), initial[index].bytes.size());
            }
        }
        m_set->addAllocation(staging);
        commitSet(device);
        MTL4::ComputeCommandEncoder* encoder = begin(device);
        if (encoder != nullptr)
        {
            const core::FormatLayout layout = core::formatLayout(shape.format);
            const bool volume = shape.type == TextureType::Tex3D;
            for (size_t index = 0; index < initial.size(); ++index)
            {
                const TextureData& data = initial[index];
                if (data.bytes.empty())
                {
                    continue;
                }
                const auto mip = static_cast<uint32_t>(index % shape.mips);
                const auto layer = static_cast<uint32_t>(index / shape.mips);
                const Extent3D extent = core::mipExtent(shape.extent, mip);
                const uint64_t rows = (uint64_t{ extent.height } + layout.blockHeight - 1) / layout.blockHeight;
                const uint64_t bytesPerImage = volume ? data.bytesPerImage : data.bytesPerRow * rows;
                encoder->copyFromBuffer(
                    staging,
                    offsets[index],
                    data.bytesPerRow,
                    bytesPerImage,
                    MTL::Size(extent.width, extent.height, volume ? extent.depth : 1),
                    destination,
                    layer,
                    mip,
                    MTL::Origin(0, 0, 0)
                );
            }
        }
        Result<void> copied = finish(device, encoder);
        m_set->removeAllocation(staging);
        commitSet(device);
        staging->release();
        if (!copied)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the copy from {} failed: {}", kStagingLabel, copied.error().message) });
        }
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::copyTexture(
        DeviceImpl& device,
        const MTL::Texture* source,
        uint32_t slice,
        uint32_t level,
        MTL::Size size,
        MTL::Buffer* destination,
        uint64_t bytesPerRow,
        uint64_t bytesPerImage
    )
    {
        std::lock_guard lock(m_mutex);
        const AutoreleasePool pool;
        m_set->addAllocation(destination);
        commitSet(device);
        MTL4::ComputeCommandEncoder* encoder = begin(device);
        if (encoder != nullptr)
        {
            encoder->copyFromTexture(source, slice, level, MTL::Origin(0, 0, 0), size, destination, 0, bytesPerRow, bytesPerImage);
        }
        Result<void> copied = finish(device, encoder);
        m_set->removeAllocation(destination);
        commitSet(device);
        return copied;
    }

    //==================================================================================================================
    void Stager::commitSet(DeviceImpl& device)
    {
        m_set->commit();
        core::DeviceCounts::add(device.core.counts().nativeHostCalls);
    }

    //==================================================================================================================
    Result<void> Stager::submitCopy(DeviceImpl& device, MTL::Buffer* source, MTL::Buffer* destination, uint64_t size)
    {
        MTL4::ComputeCommandEncoder* encoder = begin(device);
        if (encoder != nullptr)
        {
            encoder->copyFromBuffer(source, 0, destination, 0, size);
        }
        return finish(device, encoder);
    }

    //==================================================================================================================
    MTL4::ComputeCommandEncoder* Stager::begin(DeviceImpl& device)
    {
        // The destination, or the heap it lies in, may have joined the device set, which the upload queue keeps attached.
        if (device.residency.commitIfDirty())
        {
            core::DeviceCounts::add(device.core.counts().nativeHostCalls);
        }
        m_allocator->reset();
        m_commands->beginCommandBuffer(m_allocator);
        m_commands->useResidencySet(m_set);
        return m_commands->computeCommandEncoder();
    }

    //==================================================================================================================
    Result<void> Stager::finish(DeviceImpl& device, MTL4::ComputeCommandEncoder* encoder)
    {
        if (encoder != nullptr)
        {
            encoder->endEncoding();
        }
        m_commands->endCommandBuffer();
        if (encoder == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "no compute encoder was created on queue rojorhi.upload" });
        }
        const std::array<const MTL4::CommandBuffer*, 1> submitted{ m_commands };
        const std::string error = commitAndWait(m_queue, submitted, "rojorhi.upload");
        core::DeviceCounts::add(device.core.counts().nativeSubmitCalls);
        if (!error.empty())
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("a copy on queue rojorhi.upload failed: {}", error) });
        }
        return {};
    }

    //==================================================================================================================
    MTL4::CommandQueue* Stager::queue() const
    {
        return m_queue;
    }

    //==================================================================================================================
    void Stager::release()
    {
        if (m_set != nullptr)
        {
            m_set->release();
            m_set = nullptr;
        }
        if (m_commands != nullptr)
        {
            m_commands->release();
            m_commands = nullptr;
        }
        if (m_allocator != nullptr)
        {
            m_allocator->release();
            m_allocator = nullptr;
        }
        if (m_queue != nullptr)
        {
            m_queue->release();
            m_queue = nullptr;
        }
    }
} // namespace rojoRHI::metal4
