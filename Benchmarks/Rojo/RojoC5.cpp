//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC5.cpp
/// @brief Implements C5's RojoRHI arm: K uploadFrameData calls of 256 bytes in one list per frame.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        class RojoC5
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC5(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            RojoContext& m_context;
            uint32_t m_k = 0;
            std::array<std::byte, kCopyBytes> m_bytes = copyPattern();
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        // C5 creates nothing: the frame arena grows during warm-up to hold K uploads per frame.
        RojoC5::RojoC5(RojoContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0))
        {
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC5::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoC5::frame(uint32_t /*index*/)
        {
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c5");
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                (void)list.uploadFrameData(m_bytes);
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
        std::expected<std::vector<Output>, std::string> RojoC5::readback()
        {
            return std::vector<Output>{};
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC5(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC5>(config, options);
    }
} // namespace rojoRHI::bench
