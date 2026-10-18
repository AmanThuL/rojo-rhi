//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC1.cpp
/// @brief Implements C1's native arm: per repetition the 64-byte root into the arena, setAddress and drawPrimitives.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"
#include "Native/NativeTarget.h"

#include <span>

namespace rojoRHI::bench {
    namespace {
        class NativeC1
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC1(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            MTL::RenderPipelineState* m_pipeline = nullptr;
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC1::NativeC1(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        // Every repetition's root starts at its own 256-byte boundary, so the arena holds exactly K of them.
        std::expected<void, std::string> NativeC1::create()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchDraw");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::RenderPipelineState*, std::string> pipeline =
                NativeTarget::pipeline(m_context, *library, "drawVertex", "drawFragment", "bench.c1");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c1", false); !target)
            {
                return target;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = uint64_t{ m_k } * NativeContext::kArenaAlignment });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC1::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The fragment entry declares the root binding too, so the table is bound to both stages.
        std::expected<FrameResult, std::string> NativeC1::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            m_lastIndex = index;

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::RenderCommandEncoder* encoder = commands->renderCommandEncoder(m_target.pass(index % NativeContext::kFrameSlots));
            encoder->setArgumentTable(slot.table, MTL::RenderStageVertex | MTL::RenderStageFragment);
            encoder->setRenderPipelineState(m_pipeline);
            encoder->setViewport(NativeTarget::kViewport);
            encoder->setScissorRect(NativeTarget::kScissor);
            calls.record += 6;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                const DrawRoot root = drawRoot(repetition);
                slot.table->setAddress(slot.push(std::as_bytes(std::span(&root, 1))), 0);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{ 0 }, NS::UInteger{ 3 });
                calls.record += 2;
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
        std::expected<std::vector<Output>, std::string> NativeC1::readback()
        {
            return m_target.read(m_context, m_lastIndex + 1, m_lastIndex % NativeContext::kFrameSlots);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC1(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC1>(config, options);
    }
} // namespace rojoRHI::bench
