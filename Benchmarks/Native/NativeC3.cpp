//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC3.cpp
/// @brief Implements C3's native arm: per repetition pipeline, cull mode and depth-stencil state of A or B, then a draw.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"
#include "Native/NativeTarget.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        // A: cull None, no depth test or write; B: cull Back, depth test Greater with writes, as the RojoRHI arm's two
        // pipelines. Metal 4 keeps both in encoder state, so A's and B's pipeline objects are alike.
        constexpr std::array<MTL::CullMode, 2> kCull{ MTL::CullModeNone, MTL::CullModeBack };

        class NativeC3
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC3(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            std::array<MTL::RenderPipelineState*, 2> m_pipelines{};
            std::array<MTL::DepthStencilState*, 2> m_depthStates{};
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC3::NativeC3(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC3::create()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchPlain");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            for (uint32_t index = 0; index < 2; ++index)
            {
                const std::expected<MTL::RenderPipelineState*, std::string> pipeline =
                    NativeTarget::pipeline(m_context, *library, "plainVertex", "plainFragment", index == 0 ? "bench.c3.a" : "bench.c3.b");
                if (!pipeline)
                {
                    return std::unexpected(pipeline.error());
                }
                m_pipelines[index] = *pipeline;
            }

            MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
            descriptor->setDepthCompareFunction(MTL::CompareFunctionAlways);
            descriptor->setDepthWriteEnabled(false);
            m_depthStates[0] = m_context.device()->newDepthStencilState(descriptor);
            descriptor->setDepthCompareFunction(MTL::CompareFunctionGreater);
            descriptor->setDepthWriteEnabled(true);
            m_depthStates[1] = m_context.device()->newDepthStencilState(descriptor);
            m_context.calls().host += 2;
            descriptor->release();
            for (MTL::DepthStencilState* state : m_depthStates)
            {
                if (state == nullptr)
                {
                    return std::unexpected(std::string("bench.c3: the device refused a depth-stencil state"));
                }
                m_context.own(state);
            }

            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c3", true); !target)
            {
                return target;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC3::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The front-facing winding is the RojoRHI pipelines' counter-clockwise for both, so it is set once with the
        // viewport and scissor. The pass's shaders read no reserved binding, so no argument table is bound.
        std::expected<FrameResult, std::string> NativeC3::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            m_lastIndex = index;

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::RenderCommandEncoder* encoder = commands->renderCommandEncoder(m_target.pass(index % NativeContext::kFrameSlots));
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            encoder->setViewport(NativeTarget::kViewport);
            encoder->setScissorRect(NativeTarget::kScissor);
            calls.record += 5;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                const uint32_t which = repetition & 1;
                encoder->setRenderPipelineState(m_pipelines[which]);
                encoder->setCullMode(kCull[which]);
                encoder->setDepthStencilState(m_depthStates[which]);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{ 0 }, NS::UInteger{ 3 });
                calls.record += 4;
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
        std::expected<std::vector<Output>, std::string> NativeC3::readback()
        {
            return m_target.read(m_context, m_lastIndex + 1, m_lastIndex % NativeContext::kFrameSlots);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC3(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC3>(config, options);
    }
} // namespace rojoRHI::bench
