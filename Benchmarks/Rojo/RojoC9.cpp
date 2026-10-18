//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC9.cpp
/// @brief Implements C9's RojoRHI arm: K lists recorded first, then per repetition one submit signalling the next value.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>
#include <span>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        // Frames in flight; frame f's lists copy into region f mod 3 of the destination.
        constexpr uint32_t kRegions = 3;

        class RojoC9
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC9(RojoContext&, const Config&);
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
            Fence m_fence;
            uint64_t m_value = 0;
            std::vector<List> m_lists;
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC9::RojoC9(RojoContext& context, const Config& config)
            : m_context(context), m_k(config.k.value_or(0)), m_regionBytes(uint64_t{ m_k } * kCopyBytes), m_lists(m_k), m_ready(create())
        {
        }

        //==============================================================================================================
        // Each list copies into its own 256 bytes and each frame slot into its own region, because two copies into the
        // same bytes need a barrier between them (VU-rojo-sync-0021).
        std::expected<void, std::string> RojoC9::create()
        {
            const std::array<std::byte, kCopyBytes> pattern = copyPattern();
            const std::expected<Buffer, std::string> source =
                m_context.buffer(BufferDesc{ .size = kCopyBytes, .memory = Memory::Gpu, .label = "bench.c9.source" }, pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<Buffer, std::string> destination =
                m_context.buffer(BufferDesc{ .size = kRegions * m_regionBytes, .memory = Memory::Gpu, .label = "bench.c9.destination" });
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            m_destination = *destination;
            const std::expected<Fence, std::string> fence = m_context.fence(0, "bench.c9");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            m_fence = *fence;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC9::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The K lists are recorded before the CPU span, which covers only the K submits; the calls counted for the frame
        // are that window's, as the span's.
        std::expected<FrameResult, std::string> RojoC9::frame(uint32_t index)
        {
            m_lastRegion = index % kRegions;
            const uint64_t region = m_lastRegion * m_regionBytes;
            m_context.beginFrame(m_k);
            for (uint32_t list = 0; list < m_k; ++list)
            {
                m_lists[list] = acquire(m_context.device(), QueueType::Graphics, "bench.c9");
                m_lists[list].copyBuffer(m_source, 0, m_destination, region + uint64_t{ list } * kCopyBytes, kCopyBytes);
                m_lists[list].end();
            }
            QueueRef queue = m_context.queue(QueueType::Graphics);

            const CallSnapshot before = m_context.callSnapshot();
            const uint64_t start = hostTicks();
            for (uint32_t list = 0; list < m_k; ++list)
            {
                const std::array<FenceValue, 1> signal{ FenceValue{ m_fence, ++m_value } };
                queue.submit(std::span<const List>(&m_lists[list], 1), {}, signal);
            }
            const uint64_t end = hostTicks();
            const CallSnapshot after = m_context.callSnapshot();

            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            // The lists' recorded calls and barriers fold into the counters at submission, inside the window; only the
            // window's queue and host calls are the submissions'.
            const Counters& afterCounters = *after.counters;
            const Counters& beforeCounters = *before.counters;
            return FrameResult{
                .cpuNs = ticksToNanoseconds(end - start),
                .gpuNs = *gpu,
                .counted = CallWindow{ .calls = (afterCounters.nativeSubmitCalls - beforeCounters.nativeSubmitCalls) +
                                                (afterCounters.nativeHostCalls - beforeCounters.nativeHostCalls) },
            };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> RojoC9::readback()
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
    ArmResult runRojoC9(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC9>(config, options);
    }
} // namespace rojoRHI::bench
