//----------------------------------------------------------------------------------------------------------------------
/// @file NativeRootSignature.cpp
/// @brief Implements the creation of a root signature of the native arm's own, for its hand-written kernels.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeRootSignature.h"

#include <cstdint>
#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::expected<ID3D12RootSignature*, std::string> createRootSignature(NativeContext& context, const D3D12_ROOT_SIGNATURE_DESC1& desc, std::string_view label)
    {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
        versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versioned.Desc_1_1 = desc;
        ComPtr<ID3D12DeviceConfiguration> configuration;
        if (const HRESULT got = context.device()->QueryInterface(IID_PPV_ARGS(&configuration)); FAILED(got))
        {
            return std::unexpected(
                std::format("root signature {}: QueryInterface(ID3D12DeviceConfiguration) failed with HRESULT 0x{:08X}", label, static_cast<uint32_t>(got))
            );
        }
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const HRESULT serialized = configuration->SerializeVersionedRootSignature(&versioned, &blob, &errors);
        ++context.calls().host;
        if (FAILED(serialized))
        {
            std::string_view detail =
                errors ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string_view();
            // The blob is a C string; its terminating NUL is not part of the message.
            while (!detail.empty() && detail.back() == '\0')
            {
                detail.remove_suffix(1);
            }
            return std::unexpected(
                std::format(
                    "root signature {}: SerializeVersionedRootSignature failed with HRESULT 0x{:08X}: {}", label, static_cast<uint32_t>(serialized), detail
                )
            );
        }
        ID3D12RootSignature* root = nullptr;
        const HRESULT created = context.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
        ++context.calls().host;
        if (FAILED(created))
        {
            return std::unexpected(std::format("root signature {}: CreateRootSignature failed with HRESULT 0x{:08X}", label, static_cast<uint32_t>(created)));
        }
        context.own(root, label);
        return root;
    }
} // namespace rojoRHI::bench
