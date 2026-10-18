//----------------------------------------------------------------------------------------------------------------------
/// @file NativeC8.cpp
/// @brief Implements C8's native arm: per repetition one render encoder with the profiling markers RojoRHI's pass writes.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuArms.h"
#include "Common/CpuRoots.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"
#include "Native/NativeTarget.h"

#include <algorithm>
#include <array>
#include <format>

namespace rojoRHI::bench {
    namespace {
        // The Metal validation layer refuses a counter heap of more than 4,096 entries, and Metal refuses a process more
        // than 32 counter heaps: three slots of 2K entries fit up to K 20,480.
        constexpr uint32_t kHeapEntries = 4096;
        constexpr uint32_t kProcessHeaps = 32;

        class NativeC8
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeC8(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            void resolve(uint32_t);

            NativeContext& m_context;
            uint32_t m_k = 0;
            NativeTarget m_target;
            MTL::RenderPipelineState* m_pipeline = nullptr;
            NS::String* m_label = nullptr;
            std::array<std::vector<MTL4::CounterHeap*>, NativeContext::kFrameSlots> m_heaps;
            std::vector<uint32_t> m_heapEntries; ///< The entries of heap h of every slot, an even number.
            std::array<bool, NativeContext::kFrameSlots> m_written{};
            uint32_t m_lastIndex = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeC8::NativeC8(NativeContext& context, const Config& config) : m_context(context), m_k(config.k.value_or(0)), m_ready(create())
        {
        }

        //==============================================================================================================
        // Each frame slot has timestamp heaps for 2K entries, in heaps of 4,096, pass i writing entries 2i and 2i + 1 of
        // the slot's entries taken in order.
        std::expected<void, std::string> NativeC8::create()
        {
            const std::expected<MTL::Library*, std::string> library = m_context.library("BenchPlain");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::RenderPipelineState*, std::string> pipeline =
                NativeTarget::pipeline(m_context, *library, "plainVertex", "plainFragment", "bench.c8");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            m_label = m_context.string("bench.c8");
            if (const std::expected<void, std::string> target = m_target.create(m_context, "bench.c8", false); !target)
            {
                return target;
            }

            const uint64_t slotEntries = uint64_t{ 2 } * m_k;
            for (uint64_t first = 0; first < slotEntries; first += kHeapEntries)
            {
                m_heapEntries.push_back(static_cast<uint32_t>(std::min<uint64_t>(kHeapEntries, slotEntries - first)));
            }
            if (NativeContext::kFrameSlots * m_heapEntries.size() > kProcessHeaps)
            {
                return std::unexpected(
                    std::format(
                        "bench.c8: {} passes need {} timestamp heaps of {} entries, over Metal's {} per process",
                        m_k,
                        NativeContext::kFrameSlots * m_heapEntries.size(),
                        kHeapEntries,
                        kProcessHeaps
                    )
                );
            }
            MTL4::CounterHeapDescriptor* descriptor = MTL4::CounterHeapDescriptor::alloc()->init();
            descriptor->setType(MTL4::CounterHeapTypeTimestamp);
            for (uint32_t slot = 0; slot < NativeContext::kFrameSlots; ++slot)
            {
                for (const uint32_t entries : m_heapEntries)
                {
                    descriptor->setCount(entries);
                    NS::Error* error = nullptr;
                    MTL4::CounterHeap* heap = m_context.device()->newCounterHeap(descriptor, &error);
                    ++m_context.calls().host;
                    if (heap == nullptr)
                    {
                        descriptor->release();
                        return std::unexpected(std::format("bench.c8: timestamp heap of slot {}: {}", slot, describe(error)));
                    }
                    m_context.own(heap);
                    heap->setLabel(m_label);
                    ++m_context.calls().host;
                    m_heaps[slot].push_back(heap);
                }
            }
            descriptor->release();
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeC8::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // The timestamps of the slot's previous frame, which completed before this frame began, as RojoRHI resolves its
        // pass timestamps when it retires a slot.
        void NativeC8::resolve(uint32_t slot)
        {
            if (!m_written[slot])
            {
                return;
            }
            for (size_t heap = 0; heap < m_heaps[slot].size(); ++heap)
            {
                (void)m_heaps[slot][heap]->resolveCounterRange(NS::Range::Make(0, m_heapEntries[heap]));
                ++m_context.calls().host;
            }
        }

        //==============================================================================================================
        // The pass's shaders read no reserved binding, so no argument table is bound. Every pass but the first opens
        // with chapter 09's lowering of RojoRHI's barrier between passes over the target: its source lies in the previous
        // encoder, so it is a consumer barrier on the new encoder, from the fragment stage to the fragment stage.
        std::expected<FrameResult, std::string> NativeC8::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];
            const uint32_t slotIndex = index % NativeContext::kFrameSlots;
            m_lastIndex = index;
            resolve(slotIndex);
            m_written[slotIndex] = true;
            const std::vector<MTL4::CounterHeap*>& heaps = m_heaps[slotIndex];
            MTL4::RenderPassDescriptor* pass = m_target.pass(slotIndex);

            size_t heapIndex = 0;
            MTL4::CounterHeap* heap = heaps[0];
            NS::UInteger entry = 0;

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            ++calls.record;
            for (uint32_t repetition = 0; repetition < m_k; ++repetition)
            {
                commands->pushDebugGroup(m_label);
                commands->writeTimestampIntoHeap(heap, entry);
                MTL4::RenderCommandEncoder* encoder = commands->renderCommandEncoder(pass);
                encoder->setLabel(m_label);
                if (repetition > 0)
                {
                    encoder->barrierAfterQueueStages(MTL::StageFragment, MTL::StageFragment, MTL4::VisibilityOptionDevice);
                    ++calls.record;
                    ++calls.barriers;
                }
                encoder->setRenderPipelineState(m_pipeline);
                encoder->setViewport(NativeTarget::kViewport);
                encoder->setScissorRect(NativeTarget::kScissor);
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{ 0 }, NS::UInteger{ 3 });
                encoder->endEncoding();
                commands->writeTimestampIntoHeap(heap, entry + 1);
                commands->popDebugGroup();
                calls.record += 11;
                entry += 2;
                if (entry == m_heapEntries[heapIndex] && heapIndex + 1 < heaps.size())
                {
                    entry = 0;
                    heap = heaps[++heapIndex];
                }
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
        std::expected<std::vector<Output>, std::string> NativeC8::readback()
        {
            return m_target.read(m_context, m_lastIndex + 1, m_lastIndex % NativeContext::kFrameSlots);
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeC8(const Config& config, const RunOptions& options)
    {
        return runArm<NativeC8>(config, options);
    }
} // namespace rojoRHI::bench
