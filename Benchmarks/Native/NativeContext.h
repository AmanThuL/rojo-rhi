//----------------------------------------------------------------------------------------------------------------------
/// @file NativeContext.h
/// @brief Declares the native arm's Metal 4 device, queues, residency, frame slots, commits and hand-counted calls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/FeedbackCollector.h"
#include "Common/RunOptions.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // The description of a Metal error for a failure message; a null error or one without a description reads as such.
    std::string describe(NS::Error* error);

    // The native calls N has made, each counted by a plain increment beside the call: record on a command buffer or
    // encoder, submit on a queue, host on the device, a residency set, an event or a heap. A barrier counts in record and
    // again in barriers. Calls on a command allocator, and property sets on descriptors and on MTL4::CommitOptions, are
    // not native calls of metric 2 and count nowhere.
    struct NativeCalls
    {
        uint64_t record = 0;
        uint64_t submit = 0;
        uint64_t host = 0;
        uint64_t barriers = 0;
    };

    // What every frame of a workload needs, fixed at setup.
    struct NativeFrameShape
    {
        uint32_t lists = 1;      ///< Command buffers recorded per frame, each with its own allocator.
        uint64_t arenaBytes = 0; ///< Frame data per frame; 0 creates no arena.
    };

    // The system device with a graphics and a compute MTL4::CommandQueue sharing one residency set, an MTL4::Compiler
    // and three frame slots; every commit carries MTL4::CommitOptions feedback reporting to the collector. It owns every
    // object created through it and releases them in reverse order of creation; a frame is waited to completion before
    // the next begins, so teardown never releases an object the GPU still uses.
    class NativeContext
    {
      public:
        static constexpr uint32_t kFrameSlots = 3;
        static constexpr uint32_t kArgumentBuffers = 4; ///< The module's reserved buffer bindings 0 to 3.
        static constexpr uint64_t kArenaAlignment = 256;

        // One frame slot: per-list allocators and command buffers, an argument table and the frame-data arena.
        struct Slot
        {
            std::vector<MTL4::CommandAllocator*> allocators;
            std::vector<MTL4::CommandBuffer*> lists;
            MTL4::ArgumentTable* table = nullptr;
            MTL::Buffer* arena = nullptr;
            std::byte* arenaData = nullptr;
            uint64_t arenaAddress = 0;
            uint64_t arenaSize = 0;
            uint64_t arenaUsed = 0;

            // Copies bytes into the arena at the next 256-byte boundary and returns their GPU address. Overrunning the
            // size fixed at setup is a harness defect and aborts.
            uint64_t push(std::span<const std::byte> bytes)
            {
                const uint64_t offset = (arenaUsed + kArenaAlignment - 1) & ~(kArenaAlignment - 1);
                if (offset + bytes.size() > arenaSize)
                {
                    std::fprintf(
                        stderr,
                        "native arena overrun: %llu bytes at %llu of %llu\n",
                        static_cast<unsigned long long>(bytes.size()),
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(arenaSize)
                    );
                    std::abort();
                }
                std::memcpy(arenaData + offset, bytes.data(), bytes.size());
                arenaUsed = offset + bytes.size();
                return arenaAddress + offset;
            }
        };

        NativeContext() = default;
        NativeContext(const NativeContext&) = delete;
        NativeContext& operator=(const NativeContext&) = delete;
        ~NativeContext();

        std::expected<void, std::string> open(const RunOptions& options);

        MTL::Device* device() const;
        MTL4::CommandQueue* graphicsQueue() const;
        MTL4::CommandQueue* computeQueue() const;
        MTL4::Compiler* compiler() const;
        MTL::ResidencySet* residency() const;
        NativeCalls& calls();
        CallSnapshot callSnapshot() const;
        FeedbackCollector& collector();

        // Setup. Each object is owned by the context; resources join the residency set at creation.
        // Takes ownership of a +1 object.
        void own(NS::Object* object);
        std::expected<MTL::Buffer*, std::string> sharedBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial = {});
        // A Private buffer; initial bytes go through a Shared staging buffer and one setup commit, waited.
        std::expected<MTL::Buffer*, std::string> privateBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial = {});
        std::expected<MTL::Texture*, std::string> texture(const MTL::TextureDescriptor* descriptor, std::string_view label);
        // Shaders/<name>.metallib beside the executable.
        std::expected<MTL::Library*, std::string> library(std::string_view name);
        // An owned function descriptor naming one function of the library.
        MTL4::LibraryFunctionDescriptor* function(MTL::Library* library, std::string_view name);
        // An owned string, for a label or debug group name set while recording.
        NS::String* string(std::string_view text);
        std::expected<MTL::ComputePipelineState*, std::string> computePipeline(MTL::Library* library, std::string_view function, std::string_view label);
        // Compiles a caller-filled descriptor with indirect command buffer support enabled, as the RojoRHI backend does.
        std::expected<MTL::RenderPipelineState*, std::string> renderPipeline(MTL4::RenderPipelineDescriptor* descriptor, std::string_view label);

        // Creates the three frame slots and commits the residency set; called once, after every resource exists.
        std::expected<void, std::string> prepareFrames(const NativeFrameShape& shape);

        // Frames. beginFrame opens an autorelease pool, resets the slot's allocators (uncounted) and arena, expects `commits`
        // feedback reports; endFrame waits for them, closes the pool and returns the frame's GPU span.
        Slot& beginFrame(uint32_t index, uint32_t commits);
        // Commits with new MTL4::CommitOptions whose feedback reports to the collector, kept until the frame's wait: an
        // options object reports only the first commit it is given. One counted queue call.
        void commit(MTL4::CommandQueue* queue, std::span<MTL4::CommandBuffer* const> buffers);
        std::expected<std::optional<double>, std::string> endFrame();

        // Outside every measurement: copies bytes of a buffer into a Shared one through one waited commit.
        std::expected<std::vector<std::byte>, std::string> readBuffer(MTL::Buffer* source, uint64_t offset, uint64_t size);

      private:
        std::expected<double, std::string> waitForCommits();
        std::expected<void, std::string> runSetupCommit(
            MTL::Buffer* source, uint64_t sourceOffset, MTL::Buffer* destination, uint64_t destinationOffset, uint64_t size
        );

        MTL::Device* m_device = nullptr;
        MTL4::CommandQueue* m_graphics = nullptr;
        MTL4::CommandQueue* m_compute = nullptr;
        MTL::ResidencySet* m_residency = nullptr;
        MTL4::Compiler* m_compiler = nullptr;
        MTL4::CommandAllocator* m_setupAllocator = nullptr;
        MTL4::CommandBuffer* m_setupList = nullptr;
        std::vector<NS::Object*> m_owned;
        std::vector<Slot> m_slots;
        std::vector<MTL4::CommitOptions*> m_pendingOptions; ///< Reserved at open, so a commit does not grow it.
        NativeCalls m_calls;
        // On the heap so an abandoned context can leave it behind for reports still in flight.
        std::unique_ptr<FeedbackCollector> m_collector = std::make_unique<FeedbackCollector>();
        void* m_framePool = nullptr;
        bool m_abandoned = false; ///< A wait timed out: the GPU may still use every object, so none is released.
    };
} // namespace rojoRHI::bench
