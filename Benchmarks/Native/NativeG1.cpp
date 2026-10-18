//----------------------------------------------------------------------------------------------------------------------
/// @file NativeG1.cpp
/// @brief Implements G1's native arm: one dispatch of 2^20 threads, each summing 16 uint4 loads through a device pointer.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

namespace rojoRHI::bench {
    namespace {
        // The root the kernel reads at buffer 0; BenchLoadsNative.metal declares the same layout.
        struct LoadsRoot
        {
            uint64_t data;
            uint64_t output;
            uint32_t divergent;
            uint32_t pad;
        };

        class NativeG1
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeG1(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            bool m_divergent = false;
            MTL::Buffer* m_output = nullptr;
            MTL::ComputePipelineState* m_pipeline = nullptr;
            MTL4::ArgumentTable* m_table = nullptr;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeG1::NativeG1(NativeContext& context, const Config& config)
            : m_context(context), m_divergent(config.pattern.value_or("") == "divergent"), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeG1::create()
        {
            const std::vector<DataEntry> table = dataTable();
            const std::expected<MTL::Buffer*, std::string> data =
                m_context.privateBuffer(table.size() * sizeof(DataEntry), "bench.g1.data", std::as_bytes(std::span(table)));
            if (!data)
            {
                return std::unexpected(data.error());
            }
            const std::expected<MTL::Buffer*, std::string> output = m_context.privateBuffer(uint64_t{ kKernelThreads } * sizeof(DataEntry), "bench.g1.output");
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;
            const LoadsRoot rootValue{ .data = (*data)->gpuAddress(), .output = m_output->gpuAddress(), .divergent = m_divergent ? 1u : 0u, .pad = 0 };
            const std::expected<MTL::Buffer*, std::string> root =
                m_context.sharedBuffer(sizeof(LoadsRoot), "bench.g1.root", std::as_bytes(std::span(&rootValue, 1)));
            if (!root)
            {
                return std::unexpected(root.error());
            }

            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchLoadsNative");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::ComputePipelineState*, std::string> pipeline = m_context.computePipeline(*library, "loadsNative", "bench.g1");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;

            MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
            descriptor->setMaxBufferBindCount(1);
            descriptor->setLabel(m_context.string("bench.g1.arguments"));
            NS::Error* error = nullptr;
            m_table = m_context.device()->newArgumentTable(descriptor, &error);
            descriptor->release();
            ++m_context.calls().host;
            if (m_table == nullptr)
            {
                return std::unexpected("G1: argument table could not be created");
            }
            m_context.own(m_table);
            m_table->setAddress((*root)->gpuAddress(), 0);
            ++m_context.calls().host;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeG1::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> NativeG1::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(m_pipeline);
            encoder->setArgumentTable(m_table);
            encoder->dispatchThreadgroups(MTL::Size::Make(kKernelThreads / kKernelGroup, 1, 1), MTL::Size::Make(kKernelGroup, 1, 1));
            encoder->endEncoding();
            commands->endCommandBuffer();
            calls.record += 7;
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
        std::expected<std::vector<Output>, std::string> NativeG1::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_output, 0, uint64_t{ kKernelThreads } * sizeof(DataEntry));
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "output", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeG1(const Config& config, const RunOptions& options)
    {
        return runArm<NativeG1>(config, options);
    }
} // namespace rojoRHI::bench
