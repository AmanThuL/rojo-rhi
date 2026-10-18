//----------------------------------------------------------------------------------------------------------------------
/// @file NativeIndirect.cpp
/// @brief Implements arm E by hand: indirect command buffers filled by a kernel and executed with a GPU-written range.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeIndirect.h"

#include <cstring>
#include <format>

namespace rojoRHI::bench {
    namespace {
        // Mirrors Root in Kernels/BenchEncode.metal; the pointers are GPU addresses.
        struct KernelRoot
        {
            uint64_t records;
            uint64_t count;
            uint64_t icb;
            uint64_t range;
            uint64_t indices;
            uint32_t maxCount;
            uint32_t padding;
        };
        static_assert(sizeof(KernelRoot) == 48);
    } // namespace

    //==================================================================================================================
    NativeIndirect::NativeIndirect(NativeContext& context) : m_context(context)
    {
    }

    //==================================================================================================================
    uint64_t NativeIndirect::element(uint32_t slot, uint32_t draw) const
    {
        return uint64_t{ draw } * NativeContext::kFrameSlots + slot;
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeIndirect::create(std::span<const CountedDraw> draws)
    {
        m_draws.assign(draws.begin(), draws.end());
        const std::expected<MTL::Library*, std::string> library = m_context.library("BenchEncode");
        if (!library)
        {
            return std::unexpected(library.error());
        }
        const std::expected<MTL::ComputePipelineState*, std::string> encodeDraw = m_context.computePipeline(*library, "encodeDraw", "bench.encodeDraw");
        if (!encodeDraw)
        {
            return std::unexpected(encodeDraw.error());
        }
        const std::expected<MTL::ComputePipelineState*, std::string> encodeIndexed =
            m_context.computePipeline(*library, "encodeIndexed", "bench.encodeIndexed");
        if (!encodeIndexed)
        {
            return std::unexpected(encodeIndexed.error());
        }
        m_encodeDraw = *encodeDraw;
        m_encodeIndexed = *encodeIndexed;

        const uint64_t elements = uint64_t{ draws.size() } * NativeContext::kFrameSlots;
        if (elements == 0)
        {
            return std::unexpected("counted draws: none to create");
        }
        const std::expected<MTL::Buffer*, std::string> inputs = m_context.sharedBuffer(elements * kInputStride, "bench.indirect.inputs");
        if (!inputs)
        {
            return std::unexpected(inputs.error());
        }
        m_inputs = *inputs;
        const std::expected<MTL::Buffer*, std::string> ranges = m_context.privateBuffer(elements * kRangeStride, "bench.indirect.ranges");
        if (!ranges)
        {
            return std::unexpected(ranges.error());
        }
        m_ranges = *ranges;

        NativeCalls& calls = m_context.calls();
        m_icbs.reserve(elements);
        for (uint32_t draw = 0; draw < draws.size(); ++draw)
        {
            const CountedDraw& counted = draws[draw];
            MTL::IndirectCommandBufferDescriptor* descriptor = MTL::IndirectCommandBufferDescriptor::alloc()->init();
            descriptor->setInheritPipelineState(true);
            descriptor->setInheritBuffers(true);
            descriptor->setCommandTypes(counted.indexed ? MTL::IndirectCommandTypeDrawIndexed : MTL::IndirectCommandTypeDraw);
            for (uint32_t slot = 0; slot < NativeContext::kFrameSlots; ++slot)
            {
                MTL::IndirectCommandBuffer* icb = m_context.device()->newIndirectCommandBuffer(descriptor, counted.maxCount, MTL::ResourceStorageModePrivate);
                ++calls.host;
                if (icb == nullptr)
                {
                    descriptor->release();
                    return std::unexpected(
                        std::format("counted draw {}: the device refused an indirect command buffer of {} commands", draw, counted.maxCount)
                    );
                }
                m_context.own(icb);
                icb->setLabel(m_context.string(std::format("bench.indirect.icb.{}.{}", draw, slot)));
                m_context.residency()->addAllocation(icb);
                calls.host += 2;
                m_icbBytes += icb->allocatedSize();
                m_icbs.push_back(icb);

                std::byte* base = static_cast<std::byte*>(m_inputs->contents()) + element(slot, draw) * kInputStride;
                const uint64_t inputAddress = m_inputs->gpuAddress() + element(slot, draw) * kInputStride;
                const KernelRoot root{
                    .records = counted.records[slot],
                    .count = counted.counts[slot],
                    .icb = inputAddress + kIcbOffset,
                    .range = m_ranges->gpuAddress() + element(slot, draw) * kRangeStride,
                    .indices = counted.indices,
                    .maxCount = counted.maxCount,
                    .padding = 0,
                };
                std::memcpy(base, &root, sizeof(root));
                const MTL::ResourceID id = icb->gpuResourceID();
                std::memcpy(base + kIcbOffset, &id, sizeof(id));
            }
            descriptor->release();
        }
        return {};
    }

    //==================================================================================================================
    void NativeIndirect::encodeKernels(MTL4::ComputeCommandEncoder* encoder, MTL4::ArgumentTable* table, uint32_t slot, uint32_t first, uint32_t count)
    {
        NativeCalls& calls = m_context.calls();
        encoder->setArgumentTable(table);
        ++calls.record;
        const MTL::ComputePipelineState* bound = nullptr;
        for (uint32_t draw = first; draw < first + count; ++draw)
        {
            const CountedDraw& counted = m_draws[draw];
            MTL::ComputePipelineState* pipeline = counted.indexed ? m_encodeIndexed : m_encodeDraw;
            if (pipeline != bound)
            {
                encoder->setComputePipelineState(pipeline);
                ++calls.record;
                bound = pipeline;
            }
            table->setAddress(m_inputs->gpuAddress() + element(slot, draw) * kInputStride, 0);
            ++calls.record;
            encoder->dispatchThreads(MTL::Size::Make(counted.maxCount, 1, 1), MTL::Size::Make(kThreadgroup, 1, 1));
            ++calls.record;
        }
    }

    //==================================================================================================================
    void NativeIndirect::barrier(MTL4::RenderCommandEncoder* encoder)
    {
        encoder->barrierAfterQueueStages(
            MTL::StageDispatch, MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment, MTL4::VisibilityOptionDevice
        );
        NativeCalls& calls = m_context.calls();
        ++calls.record;
        ++calls.barriers;
    }

    //==================================================================================================================
    void NativeIndirect::execute(MTL4::RenderCommandEncoder* encoder, uint32_t slot, uint32_t draw)
    {
        const uint64_t index = element(slot, draw);
        encoder->executeCommandsInBuffer(m_icbs[index], m_ranges->gpuAddress() + index * kRangeStride);
        ++m_context.calls().record;
    }

    //==================================================================================================================
    uint64_t NativeIndirect::indirectCommandBufferBytes() const
    {
        return m_icbBytes;
    }
} // namespace rojoRHI::bench
