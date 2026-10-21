//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC7.cpp
/// @brief Implements C7's native arm on D3D12: per repetition one 256-byte copy between buffers.
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
            NativeBuffer m_source;
            NativeBuffer m_destination;
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
            const std::expected<NativeBuffer, std::string> source = m_context.defaultBuffer(kCopyBytes, "bench.c7.source", pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<NativeBuffer, std::string> destination =
                m_context.defaultBuffer(NativeContext::kFrameSlots * m_regionBytes, "bench.c7.destination");
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
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            m_lastRegion = index % NativeContext::kFrameSlots;
            const uint64_t region = m_lastRegion * m_regionBytes;

            const uint64_t start = hostTicks();
            if (const std::expected<void, std::string> reset = resetList(m_context, slot, 0); !reset)
            {
                return std::unexpected(reset.error());
            }
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                list->CopyBufferRegion(m_destination.resource, region + uint64_t{ repetition } * kCopyBytes, m_source.resource, 0, kCopyBytes);
                ++calls.record;
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
        std::expected<std::vector<Output>, std::string> NativeC7::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = readBufferAfter(
                m_context, m_destination.resource, m_lastRegion * m_regionBytes, m_regionBytes, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST
            );
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
