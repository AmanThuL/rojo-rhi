//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC9.cpp
/// @brief Implements C9's native arm: K command buffers recorded first, then per repetition commit and signalEvent.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

#include <array>
#include <span>

namespace rojoRHI::bench {
    namespace {
        class NativeC9
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC9(NativeContext&, const Config&);
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
            MTL::SharedEvent* m_event = nullptr;
            uint64_t m_value = 0;
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC9::NativeC9(NativeContext& context, const Config& config)
            : m_context(context), m_k(config.k.value_or(0)), m_regionBytes(uint64_t{ m_k } * kCopyBytes), m_ready(create())
        {
        }

        //==============================================================================================================
        // The destination holds one region of K blocks per frame slot, as the RojoRHI arm's does: list i of frame f copies
        // into block i of region f mod 3. Each frame slot has K command buffers, each with its own allocator.
        std::expected<void, std::string> NativeC9::create()
        {
            const std::array<std::byte, kCopyBytes> pattern = copyPattern();
            const std::expected<MTL::Buffer*, std::string> source = m_context.privateBuffer(kCopyBytes, "bench.c9.source", pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<MTL::Buffer*, std::string> destination =
                m_context.privateBuffer(NativeContext::kFrameSlots * m_regionBytes, "bench.c9.destination");
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            m_destination = *destination;
            m_event = m_context.device()->newSharedEvent();
            ++m_context.calls().host;
            if (m_event == nullptr)
            {
                return std::unexpected(std::string("bench.c9: the device refused a shared event"));
            }
            m_context.own(m_event);
            m_event->setLabel(m_context.string("bench.c9"));
            ++m_context.calls().host;
            return m_context.prepareFrames(NativeFrameShape{ .lists = m_k });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC9::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The K command buffers are recorded before the CPU span, which covers only the K commits and their signals; the
        // calls counted for the frame are that window's, as the span's.
        std::expected<FrameResult, std::string> NativeC9::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, m_k);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandQueue* queue = m_context.graphicsQueue();
            m_lastRegion = index % NativeContext::kFrameSlots;
            const uint64_t region = m_lastRegion * m_regionBytes;
            for (uint32_t list = 0; list < m_k; ++list)
            {
                MTL4::CommandBuffer* commands = slot.lists[list];
                commands->beginCommandBuffer(slot.allocators[list]);
                MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
                encoder->copyFromBuffer(m_source, 0, m_destination, region + uint64_t{ list } * kCopyBytes, kCopyBytes);
                encoder->endEncoding();
                commands->endCommandBuffer();
                calls.record += 5;
            }

            const CallSnapshot before = m_context.callSnapshot();
            const uint64_t start = hostTicks();
            for (uint32_t list = 0; list < m_k; ++list)
            {
                m_context.commit(queue, std::span<MTL4::CommandBuffer* const>(&slot.lists[list], 1));
                queue->signalEvent(m_event, ++m_value);
                ++calls.submit;
            }
            const uint64_t end = hostTicks();
            const CallSnapshot after = m_context.callSnapshot();

            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{
                .cpuNs = ticksToNanoseconds(end - start),
                .gpuNs = *gpu,
                .counted = CallWindow{ .calls = after.calls - before.calls, .barriers = after.barriers - before.barriers },
            };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeC9::readback()
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
    ArmResult runNativeC9(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC9>(config, options);
    }
} // namespace rojoRHI::bench
