//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC10.cpp
/// @brief Implements C10's native arm: counted draws encoded by kernels batched ahead of the render encoder, and C10m1's plain indirect draws.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CountedArms.h"
#include "Common/CountedData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"
#include "Native/NativeIndirect.h"

#include <array>
#include <cstring>

namespace rojoRHI::bench {
    namespace {
        // C10 runs counted draws through the encode kernel; C10m1 (a null count over one record) is one native indirect
        // draw each, from a record in Metal's own layout. A configuration whose pattern is "indexed" draws UInt16 indexed
        // records instead of C10's; no registered row does, and a check does.
        class NativeC10
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC10(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            std::expected<void, std::string> createPipeline();
            std::expected<void, std::string> createCountedDraws();

            NativeContext& m_context;
            NativeIndirect m_indirect;
            uint32_t m_k = 0;
            uint32_t m_maxCount = 0;
            bool m_nullCount = false;
            bool m_indexed = false;
            MTL::Texture* m_target = nullptr;
            MTL4::RenderPassDescriptor* m_pass = nullptr;
            MTL::RenderPipelineState* m_pipeline = nullptr;
            MTL::Buffer* m_nativeRecord = nullptr;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC10::NativeC10(NativeContext& context, const Config& config)
            : m_context(context), m_indirect(context), m_k(config.k.value_or(0)), m_maxCount(config.maxCount.value_or(kCountedMaxCount)),
              m_nullCount(config.nullCount.value_or(false)), m_indexed(config.pattern == "indexed"), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC10::createPipeline()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchPoints");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
            descriptor->setVertexFunctionDescriptor(m_context.function(*library, "pointVertex"));
            descriptor->setFragmentFunctionDescriptor(m_context.function(*library, "pointFragment"));
            descriptor->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR32Float);
            const std::expected<MTL::RenderPipelineState*, std::string> pipeline = m_context.renderPipeline(descriptor, "bench.c10.pipeline");
            descriptor->release();
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        // Every counted draw reads the same records and count in every slot, so one buffer of each serves them all.
        std::expected<void, std::string> NativeC10::createCountedDraws()
        {
            const std::vector<std::byte> records = m_indexed ? indexedRecords(m_maxCount) : drawRecords(m_maxCount);
            const std::expected<MTL::Buffer*, std::string> recordBuffer = m_context.sharedBuffer(records.size(), "bench.c10.records", records);
            if (!recordBuffer)
            {
                return std::unexpected(recordBuffer.error());
            }
            const std::array<uint32_t, 1> countValue{ m_maxCount };
            const std::expected<MTL::Buffer*, std::string> countBuffer =
                m_context.sharedBuffer(sizeof(uint32_t), "bench.c10.count", std::as_bytes(std::span(countValue)));
            if (!countBuffer)
            {
                return std::unexpected(countBuffer.error());
            }
            const std::array<uint16_t, 1> index{ 0 };
            const std::expected<MTL::Buffer*, std::string> indexBuffer =
                m_context.sharedBuffer(sizeof(uint16_t), "bench.c10.indices", std::as_bytes(std::span(index)));
            if (!indexBuffer)
            {
                return std::unexpected(indexBuffer.error());
            }

            CountedDraw draw{ .indices = (*indexBuffer)->gpuAddress(), .maxCount = m_maxCount, .indexed = m_indexed };
            draw.records.fill((*recordBuffer)->gpuAddress());
            draw.counts.fill((*countBuffer)->gpuAddress());
            const std::vector<CountedDraw> draws(m_k, draw);
            return m_indirect.create(draws);
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeC10::create()
        {
            if (const std::expected<void, std::string> piped = createPipeline(); !piped)
            {
                return piped;
            }
            MTL::TextureDescriptor* textureDescriptor =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR32Float, kCountedTargetEdge, kCountedTargetEdge, false);
            textureDescriptor->setStorageMode(MTL::StorageModeShared);
            textureDescriptor->setUsage(MTL::TextureUsageRenderTarget);
            const std::expected<MTL::Texture*, std::string> target = m_context.texture(textureDescriptor, "bench.c10.target");
            if (!target)
            {
                return std::unexpected(target.error());
            }
            m_target = *target;

            m_pass = MTL4::RenderPassDescriptor::alloc()->init();
            m_context.own(m_pass);
            MTL::RenderPassColorAttachmentDescriptor* attachment = m_pass->colorAttachments()->object(0);
            attachment->setTexture(m_target);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearColor(MTL::ClearColor::Make(0, 0, 0, 0));

            if (m_nullCount)
            {
                const MTL::DrawPrimitivesIndirectArguments record{ .vertexCount = 1, .instanceCount = 1, .vertexStart = 0, .baseInstance = kSingleDrawId };
                const std::expected<MTL::Buffer*, std::string> buffer =
                    m_context.sharedBuffer(sizeof(record), "bench.c10.nativeRecord", std::as_bytes(std::span(&record, 1)));
                if (!buffer)
                {
                    return std::unexpected(buffer.error());
                }
                m_nativeRecord = *buffer;
            }
            else if (const std::expected<void, std::string> counted = createCountedDraws(); !counted)
            {
                return counted;
            }
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC10::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> NativeC10::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            const uint32_t slotIndex = index % NativeContext::kFrameSlots;
            const MTL::Viewport viewport{ 0, 0, double{ kCountedTargetEdge }, double{ kCountedTargetEdge }, 0, 1 };

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            ++calls.record;
            if (!m_nullCount)
            {
                MTL4::ComputeCommandEncoder* kernels = commands->computeCommandEncoder();
                ++calls.record;
                m_indirect.encodeKernels(kernels, slot.table, slotIndex, 0, m_k);
                kernels->endEncoding();
                ++calls.record;
            }
            MTL4::RenderCommandEncoder* encoder = commands->renderCommandEncoder(m_pass);
            encoder->setRenderPipelineState(m_pipeline);
            encoder->setViewport(viewport);
            encoder->setScissorRect(MTL::ScissorRect{ 0, 0, kCountedTargetEdge, kCountedTargetEdge });
            calls.record += 4;
            if (m_nullCount)
            {
                for (uint32_t draw = 0; draw < m_k; ++draw)
                {
                    encoder->drawPrimitives(MTL::PrimitiveTypePoint, m_nativeRecord->gpuAddress());
                    ++calls.record;
                }
            }
            else
            {
                m_indirect.barrier(encoder);
                for (uint32_t draw = 0; draw < m_k; ++draw)
                {
                    m_indirect.execute(encoder, slotIndex, draw);
                }
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
        // The target is Shared and the last frame completed, so the host reads it directly.
        std::expected<std::vector<Output>, std::string> NativeC10::readback()
        {
            std::vector<std::byte> bytes(kCountedTargetBytes);
            m_target->getBytes(bytes.data(), kCountedTargetEdge * sizeof(float), MTL::Region::Make2D(0, 0, kCountedTargetEdge, kCountedTargetEdge), 0);
            std::vector<Output> outputs;
            outputs.push_back(Output{ "target", std::move(bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC10(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC10>(config, options);
    }
} // namespace rojoRHI::bench
