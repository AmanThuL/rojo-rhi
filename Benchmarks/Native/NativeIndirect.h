//----------------------------------------------------------------------------------------------------------------------
/// @file NativeIndirect.h
/// @brief Declares arm E by hand: the kernel that encodes a counted draw into an indirect command buffer and the commands that run it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Native/NativeContext.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // One counted draw as the encode kernel reads it. Records are chapter 08's DrawIndirectArgs, or DrawIndexedIndirectArgs
    // when indexed, with the index buffer's UInt16 indices at `indices`. A draw may read different records and a different
    // count in each frame slot; a null count (address 0) means exactly maxCount.
    struct CountedDraw
    {
        std::array<uint64_t, NativeContext::kFrameSlots> records{};
        std::array<uint64_t, NativeContext::kFrameSlots> counts{};
        uint64_t indices = 0;
        uint32_t maxCount = 0;
        bool indexed = false;
    };

    // Every counted draw of a workload, each with its own indirect command buffer and execution range in every frame
    // slot: a pass batches the kernels of its draws in one compute encoder ahead of its render encoder, as RojoRHI's
    // backend does, then runs one barrier and one executeCommandsInBuffer per draw. Every native call made while
    // recording is counted in the context (a barrier in both record and barriers); setup calls count as host calls.
    class NativeIndirect
    {
      public:
        explicit NativeIndirect(NativeContext&);

        // Setup, before NativeContext::prepareFrames: loads the kernels, creates the indirect command buffers, the
        // ranges and the kernel inputs, and adds them to the residency set. Draw i is `draws[i]` below.
        std::expected<void, std::string> create(std::span<const CountedDraw> draws);

        // Encodes the kernels of draws [first, first + count) in the open compute encoder: one pipeline bind per
        // kind change, the argument table once, then per draw one table write and one dispatch. The caller opens and
        // ends the encoder, and `table` is the slot's argument table.
        void encodeKernels(MTL4::ComputeCommandEncoder* encoder, MTL4::ArgumentTable* table, uint32_t slot, uint32_t first, uint32_t count);

        // One barrierAfterQueueStages from the dispatch stage to the vertex, object, mesh and fragment stages with
        // device visibility, ordering every kernel's writes before the draws that execute them.
        void barrier(MTL4::RenderCommandEncoder* encoder);

        // executeCommandsInBuffer of draw `draw` with the range its kernel wrote.
        void execute(MTL4::RenderCommandEncoder* encoder, uint32_t slot, uint32_t draw);

        // Allocated bytes of every indirect command buffer created.
        uint64_t indirectCommandBufferBytes() const;

      private:
        // The kernel's inputs for one draw in one slot, 256-byte aligned; the indirect command buffer's resource ID
        // follows at kIcbOffset. Mirrors Root in Kernels/BenchEncode.metal.
        static constexpr uint64_t kInputStride = 256;
        static constexpr uint64_t kIcbOffset = 64;
        static constexpr uint64_t kRangeStride = 256;
        static constexpr uint32_t kThreadgroup = 64;

        uint64_t element(uint32_t slot, uint32_t draw) const;

        NativeContext& m_context;
        MTL::ComputePipelineState* m_encodeDraw = nullptr;
        MTL::ComputePipelineState* m_encodeIndexed = nullptr;
        MTL::Buffer* m_inputs = nullptr;
        MTL::Buffer* m_ranges = nullptr;
        std::vector<MTL::IndirectCommandBuffer*> m_icbs; ///< Draw-major, one per slot.
        std::vector<CountedDraw> m_draws;
        uint64_t m_icbBytes = 0;
    };
} // namespace rojoRHI::bench
