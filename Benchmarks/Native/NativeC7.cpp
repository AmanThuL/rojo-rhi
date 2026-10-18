//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC7.cpp
/// @brief Implements C7's native arm: K copyFromBuffer calls of 256 bytes on one compute encoder per frame.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

namespace rojoRHI::bench {
    namespace {
        class NativeC7
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC7(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            uint64_t m_regionBytes = 0;
            MTL::Buffer* m_source = nullptr;
            MTL::Buffer* m_destination = nullptr;
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC7::NativeC7(NativeContext& context, const Config& config)
            : m_context(context), m_k(config.k.value_or(0)), m_regionBytes(uint64_t{ m_k } * kCopyBytes), m_ready(create())
        {
        }

        //==============================================================================================================
        // The destination holds one region of K blocks per frame slot, as the RojoRHI arm's does: repetition i of frame
        // f writes block i of region f mod 3.
        std::expected<void, std::string> NativeC7::create()
        {
            const std::array<std::byte, kCopyBytes> pattern = copyPattern();
            const std::expected<MTL::Buffer*, std::string> source = m_context.privateBuffer(kCopyBytes, "bench.c7.source", pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<MTL::Buffer*, std::string> destination =
                m_context.privateBuffer(NativeContext::kFrameSlots * m_regionBytes, "bench.c7.destination");
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            m_destination = *destination;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC7::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> NativeC7::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            m_lastRegion = index % NativeContext::kFrameSlots;
            const uint64_t region = m_lastRegion * m_regionBytes;

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            calls.record += 2;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                encoder->copyFromBuffer(m_source, 0, m_destination, region + uint64_t{ repetition } * kCopyBytes, kCopyBytes);
                ++calls.record;
            }
            encoder->endEncoding();
            commands->endCommandBuffer();
            calls.record += 2;
            const uint64_t end = hostTicks();

            MTL4::CommandBuffer* const buffers[] = { commands };
            m_context.commit(m_context.graphicsQueue(), buffers);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeC7::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_destination, m_lastRegion * m_regionBytes, m_regionBytes);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "dst", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC7(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC7>(config, options);
    }
} // namespace rojoRHI::bench
