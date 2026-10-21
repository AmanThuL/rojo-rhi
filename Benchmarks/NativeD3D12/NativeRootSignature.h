//----------------------------------------------------------------------------------------------------------------------
/// @file NativeRootSignature.h
/// @brief Declares the creation of a root signature of the native arm's own, for its hand-written kernels.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "NativeD3D12/NativeContext.h"

#include <expected>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    // Serializes `desc` as a version 1.1 root signature through the device's configuration interface, as the context's
    // global root signature is, and creates it, owned by the context and named `label`: two counted host calls.
    std::expected<ID3D12RootSignature*, std::string> createRootSignature(
        NativeContext& context, const D3D12_ROOT_SIGNATURE_DESC1& desc, std::string_view label
    );
} // namespace rojoRHI::bench
