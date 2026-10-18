//----------------------------------------------------------------------------------------------------------------------
/// @file RojoContext.h
/// @brief Declares the RojoRHI arm's device with the round's fixed description, its frame sequence and owned objects.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/FeedbackCollector.h"
#include "Common/RunOptions.h"
#include "Rojo/RojoCalls.h"
#include "Rojo/RojoGpuTiming.h"
#include "Rojo/RojoShaders.h"

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // A device created with framesInFlight 3, validation from the run options, enableCapture false, an empty pipeline
    // cache and the label "bench". It owns every object created through it and destroys them, newest first, before
    // destroyDevice, so a validating device reports no live object.
    class RojoContext
    {
      public:
        RojoContext() = default;
        RojoContext(const RojoContext&) = delete;
        RojoContext& operator=(const RojoContext&) = delete;
        ~RojoContext();

        std::expected<void, std::string> open(const RunOptions& options);

        Device device() const;
        QueueRef queue(QueueType type) const;
        CallSnapshot callSnapshot() const;

        // Frames. beginFrame expects `commits` commit reports and calls Device::beginFrame; endFrame calls
        // Device::endFrame, then waits for those reports where the backend gives them (Metal 4) and returns the frame's
        // GPU span; on Null a submission completes inside the call and the span is empty.
        void beginFrame(uint32_t commits);
        std::expected<std::optional<double>, std::string> endFrame();

        // Logs RojoRHI's calls from the next beginFrame until traceFrame(false); Null builds only record through the
        // tracing wrappers.
        void traceFrame(bool on);
        std::span<const std::string_view> tracedCalls() const;

        // Owned objects, each named in its error.
        std::expected<Buffer, std::string> buffer(const BufferDesc& desc, std::span<const std::byte> initial = {});
        std::expected<Texture, std::string> texture(const TextureDesc& desc, std::span<const TextureData> initial = {});
        std::expected<ViewIndex, std::string> view(Texture texture, const TextureViewDesc& desc);
        std::expected<SamplerIndex, std::string> sampler(const SamplerDesc& desc);
        std::expected<ShaderLibrary, std::string> library(std::string_view name, std::span<const ShaderEntryName> entries);
        std::expected<Pipeline, std::string> pipeline(const GraphicsPipelineDesc& desc);
        std::expected<Pipeline, std::string> pipeline(const ComputePipelineDesc& desc);
        std::expected<Fence, std::string> fence(uint64_t initial, std::string_view label);

        // Outside every measurement: one frame that records `before`, copies the bytes into a Readback buffer and
        // signals a fence the CPU waits on before reading them.
        std::expected<std::vector<std::byte>, std::string> readBuffer(Buffer source, uint64_t offset, uint64_t size, const GlobalBarrier& before);

      private:
        enum class Kind : uint8_t
        {
            Buffer,
            Texture,
            View,
            Sampler,
            Library,
            Pipeline,
            Fence,
        };

        struct Owned
        {
            Kind kind;
            uint64_t bits;
        };

        Device m_device;
        bool m_open = false;
        bool m_timed = false;
        bool m_tracing = false;
        bool m_abandoned = false; ///< A wait failed: the GPU may still use every object, so teardown releases nothing.
        // On the heap so an abandoned context can leave both behind for reports still in flight.
        std::unique_ptr<FeedbackCollector> m_collector = std::make_unique<FeedbackCollector>();
        std::unique_ptr<RojoGpuTiming> m_timing = std::make_unique<RojoGpuTiming>(*m_collector);
        std::vector<Owned> m_owned;
        Fence m_readbackFence;
        uint64_t m_readbackValue = 0;
    };
} // namespace rojoRHI::bench
