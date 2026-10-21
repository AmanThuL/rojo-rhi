//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC5.cpp
/// @brief Implements C5's native arm on D3D12: per repetition 256 bytes of frame data into the arena, no native call.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

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
            ID3D12GraphicsCommandList9* list = slot.lists[0];

            const uint64_t start = hostTicks();
            if (const std::expected<void, std::string> reset = resetList(m_context, slot, 0); !reset)
            {
                return std::unexpected(reset.error());
            }
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                (void)slot.push(m_bytes);
            }
            if (const std::expected<void, std::string> closed = closeList(m_context, list); !closed)
            {
                return std::unexpected(closed.error());
            }
            const uint64_t end = hostTicks();

            ID3D12CommandList* const lists[] = { list };
            m_context.commit(m_context.graphicsQueue(), lists);
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
