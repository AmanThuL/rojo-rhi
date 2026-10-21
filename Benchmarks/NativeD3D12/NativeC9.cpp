//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC9.cpp
/// @brief Implements C9's native arm on D3D12: K pre-recorded lists, each executed and followed by a fence signal.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeTarget.h"

#include <array>
#include <format>
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
            NativeBuffer m_source;
            NativeBuffer m_destination;
            ID3D12Fence* m_fence = nullptr;
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
        // into block i of region f mod 3. Each frame slot has K lists, each with its own allocator.
        std::expected<void, std::string> NativeC9::create()
        {
            const std::array<std::byte, kCopyBytes> pattern = copyPattern();
            const std::expected<NativeBuffer, std::string> source = m_context.defaultBuffer(kCopyBytes, "bench.c9.source", pattern);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            m_source = *source;
            const std::expected<NativeBuffer, std::string> destination =
                m_context.defaultBuffer(NativeContext::kFrameSlots * m_regionBytes, "bench.c9.destination");
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            m_destination = *destination;
            const std::expected<ID3D12Fence*, std::string> fence = m_context.fence("bench.c9");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            m_fence = *fence;
            return m_context.prepareFrames(NativeFrameShape{ .lists = m_k });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC9::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The K lists are recorded before the CPU span, which covers only the K executions and their signals; the calls
        // counted for the frame are that window's, as the span's.
        std::expected<FrameResult, std::string> NativeC9::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, m_k);
            NativeCalls& calls = m_context.calls();
            ID3D12CommandQueue* queue = m_context.graphicsQueue();
            m_lastRegion = index % NativeContext::kFrameSlots;
            const uint64_t region = m_lastRegion * m_regionBytes;
            for (uint32_t list = 0; list < m_k; ++list)
            {
                if (const std::expected<void, std::string> reset = resetList(m_context, slot, list); !reset)
                {
                    return std::unexpected(reset.error());
                }
                slot.lists[list]->CopyBufferRegion(m_destination.resource, region + uint64_t{ list } * kCopyBytes, m_source.resource, 0, kCopyBytes);
                ++calls.record;
                if (const std::expected<void, std::string> closed = closeList(m_context, slot.lists[list]); !closed)
                {
                    return std::unexpected(closed.error());
                }
            }

            HRESULT signalFailure = S_OK;
            const CallSnapshot before = m_context.callSnapshot();
            const uint64_t start = hostTicks();
            for (uint32_t list = 0; list < m_k; ++list)
            {
                ID3D12CommandList* const submitted[] = { slot.lists[list] };
                m_context.commit(queue, submitted);
                // A skipped commit leaves the signal's value unreached by any list, which is safe: the frame fence's signal follows on
                // the same queue and endFrame fails the frame.
                const HRESULT signalled = queue->Signal(m_fence, ++m_value);
                if (FAILED(signalled) && SUCCEEDED(signalFailure))
                {
                    signalFailure = signalled;
                }
                ++calls.submit;
            }
            const uint64_t end = hostTicks();
            const CallSnapshot after = m_context.callSnapshot();

            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            if (FAILED(signalFailure))
            {
                return std::unexpected(std::format("ID3D12CommandQueue::Signal of the C9 fence failed (0x{:08X})", static_cast<uint32_t>(signalFailure)));
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
    ArmResult runNativeC9(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC9>(config, options);
    }
} // namespace rojoRHI::bench
