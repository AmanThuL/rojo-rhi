//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Views.h
/// @brief Declares the Metal 4 sampler descriptor and the retire hooks of view and sampler slots.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <string_view>

namespace rojoRHI::metal4 {
    // Returns +1; the caller releases it and holds an autorelease pool.
    MTL::SamplerDescriptor* samplerDescriptor(const SamplerDesc& desc, std::string_view label);

    // Runs under the view heap's lock.
    void retireView(void* context, void* native);

    // Runs under the sampler heap's lock.
    void retireSampler(void* context, void* native);
} // namespace rojoRHI::metal4
