//----------------------------------------------------------------------------------------------------------------------
/// @file NativeIndirect.h
/// @brief Declares the native arm's counted draws: the command signatures that write each record's drawId to b1, and ExecuteIndirect.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "NativeD3D12/NativeContext.h"

#include <cstdint>
#include <expected>
#include <string>

namespace rojoRHI::bench {
    // The byte strides of the records R writes, chapter 08's DrawIndirectArgs and DrawIndexedIndirectArgs: the drawId, then
    // D3D12_DRAW_ARGUMENTS or D3D12_DRAW_INDEXED_ARGUMENTS.
    inline constexpr uint32_t kDrawRecordStride = 20;
    inline constexpr uint32_t kIndexedRecordStride = 24;

    // One counted draw: up to maxCount records at `records`, capped by the uint32 at `count` when `count` is not null, else
    // exactly maxCount. Records are DrawIndirectArgs, or DrawIndexedIndirectArgs when indexed.
    struct CountedDraw
    {
        ID3D12Resource* records = nullptr;
        uint64_t recordsOffset = 0;
        ID3D12Resource* count = nullptr;
        uint64_t countOffset = 0;
        uint32_t maxCount = 0;
        bool indexed = false;
    };

    // The draw and indexed-draw command signatures, created on the context's global root signature: a CONSTANT argument
    // writes each record's leading uint to the root constant at b1 (root parameter 1, offset 0, one value), which the
    // module's drawId reads, then a DRAW or DRAW_INDEXED argument reads the rest of the record. A counted draw is one
    // ExecuteIndirect inside the render pass, which takes the count buffer natively.
    class NativeIndirect
    {
      public:
        explicit NativeIndirect(NativeContext&);

        // Setup: creates both command signatures, owned by the context; one counted host call each.
        std::expected<void, std::string> create();

        // Records ExecuteIndirect of `draw` on the signature of its kind: one counted record call. The caller has set the
        // global root signature, a graphics pipeline and, for an indexed draw, the index buffer.
        void execute(ID3D12GraphicsCommandList9& list, const CountedDraw& draw);

      private:
        NativeContext& m_context;
        ID3D12CommandSignature* m_draw = nullptr;
        ID3D12CommandSignature* m_indexed = nullptr;
    };
} // namespace rojoRHI::bench
