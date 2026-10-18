//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC7.cpp
/// @brief Implements C7's RojoRHI arm: K copyBuffer calls of 256 bytes between Gpu buffers in one list per frame.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        // Frames in flight; frame f's copies land in region f mod 3 of the destination.
        constexpr uint32_t kRegions = 3;

        class RojoC7
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC7(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            uint64_t m_regionBytes = 0;
            Buffer m_source;
            Buffer m_destination;
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC7::RojoC7(RojoContext& context, const Config& config)
            : m_context(context), m_k(config.k.value_or(0)), m_regionBytes(uint64_t{ m_k } * kCopyBytes), m_ready(create())
        {
        }

        //==============================================================================================================
        // Each repetition writes its own 256 bytes and each frame slot its own region, because two copies into the
        // same bytes need a barrier between them (VU-rojo-sync-0021): a frame's copies are ordered after the copies of
        // the frame three earlier only once beginFrame has retired that frame's slot.
        std::expected<void, std::string> RojoC7::create()
        {
            const std::array<std::byte, kCopyBytes> pattern = copyPattern();
            const std::expected<Buffer, std::string> source =
                m_context.buffer(BufferDesc{ .size = kCopyBytes, .memory = Memory::Gpu, .label = "bench.c7.source" }, pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<Buffer, std::string> destination =
                m_context.buffer(BufferDesc{ .size = kRegions * m_regionBytes, .memory = Memory::Gpu, .label = "bench.c7.destination" });
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            m_destination = *destination;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC7::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoC7::frame(uint32_t index)
        {
            m_lastRegion = index % kRegions;
            const uint64_t region = m_lastRegion * m_regionBytes;
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c7");
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list.copyBuffer(m_source, 0, m_destination, region + uint64_t{ repetition } * kCopyBytes, kCopyBytes);
            }
            list.end();
            const uint64_t end = hostTicks();
            const std::array<List, 1> lists{ list };
            m_context.queue(QueueType::Graphics).submit(lists);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> RojoC7::readback()
        {
            const GlobalBarrier copied{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead };
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_destination, m_lastRegion * m_regionBytes, m_regionBytes, copied);
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
    ArmResult runRojoC7(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC7>(config, options);
    }
} // namespace rojoRHI::bench
