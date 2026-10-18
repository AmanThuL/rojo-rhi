//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC5.cpp
/// @brief Implements C5's native arm: K copies of 256 bytes into the frame-data arena, no native call per repetition.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        class NativeC5
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC5(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            NativeContext& m_context;
            uint32_t m_k = 0;
            std::array<std::byte, kCopyBytes> m_bytes = copyPattern();
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC5::NativeC5(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0))
        {
            // Every repetition's bytes start at their own 256-byte boundary, so the arena holds exactly K blocks.
            m_ready = m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = uint64_t{ m_k } * kCopyBytes });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC5::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> NativeC5::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                (void)slot.push(m_bytes);
            }
            commands->endCommandBuffer();
            ++calls.record;
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
        std::expected<std::vector<Output>, std::string> NativeC5::readback()
        {
            return std::vector<Output>{};
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC5(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC5>(config, options);
    }
} // namespace rojoRHI::bench
