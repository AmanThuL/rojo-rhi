//----------------------------------------------------------------------------------------------------------------------
/// @file NativeIndirect.cpp
/// @brief Implements the native arm's counted draws: the draw command signatures and ExecuteIndirect.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeIndirect.h"

#include <rojoRHI/RHI.h>

#include <format>
#include <string_view>

namespace rojoRHI::bench {
    static_assert(sizeof(DrawIndirectArgs) == kDrawRecordStride);
    static_assert(sizeof(DrawIndexedIndirectArgs) == kIndexedRecordStride);

    namespace {
        // The global root signature's root constant at b1, the draw identifier.
        constexpr UINT kDrawIdParameter = 1;

        //==============================================================================================================
        // The command signature whose records are the drawId, then the arguments of `command`, `stride` bytes in all.
        std::expected<ID3D12CommandSignature*, std::string> drawCommand(
            NativeContext& context, D3D12_INDIRECT_ARGUMENT_TYPE command, UINT stride, std::string_view label
        )
        {
            D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
            arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            arguments[0].Constant.RootParameterIndex = kDrawIdParameter;
            arguments[0].Constant.DestOffsetIn32BitValues = 0;
            arguments[0].Constant.Num32BitValuesToSet = 1;
            arguments[1].Type = command;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = stride;
            desc.NumArgumentDescs = 2;
            desc.pArgumentDescs = arguments;
            desc.NodeMask = 0;
            ID3D12CommandSignature* signature = nullptr;
            const HRESULT created = context.device()->CreateCommandSignature(&desc, context.rootSignature(), IID_PPV_ARGS(&signature));
            ++context.calls().host;
            if (FAILED(created))
            {
                return std::unexpected(std::format("CreateCommandSignature for {} failed with HRESULT 0x{:08X}", label, static_cast<uint32_t>(created)));
            }
            context.own(signature, label);
            return signature;
        }
    } // namespace

    //==================================================================================================================
    NativeIndirect::NativeIndirect(NativeContext& context) : m_context(context)
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeIndirect::create()
    {
        const std::expected<ID3D12CommandSignature*, std::string> draw =
            drawCommand(m_context, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, kDrawRecordStride, "bench.signature.draw");
        if (!draw)
        {
            return std::unexpected(draw.error());
        }
        const std::expected<ID3D12CommandSignature*, std::string> indexed =
            drawCommand(m_context, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, kIndexedRecordStride, "bench.signature.indexed");
        if (!indexed)
        {
            return std::unexpected(indexed.error());
        }
        m_draw = *draw;
        m_indexed = *indexed;
        return {};
    }

    //==================================================================================================================
    void NativeIndirect::execute(ID3D12GraphicsCommandList9& list, const CountedDraw& draw)
    {
        list.ExecuteIndirect(draw.indexed ? m_indexed : m_draw, draw.maxCount, draw.records, draw.recordsOffset, draw.count, draw.countOffset);
        ++m_context.calls().record;
    }
} // namespace rojoRHI::bench
