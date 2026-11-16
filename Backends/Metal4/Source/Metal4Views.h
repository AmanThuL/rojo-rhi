//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Views.h
/// @brief Declares the Metal 4 sampler descriptor and the retire hooks of view and sampler slots.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <string_view>

namespace rojoRHI::metal4 {
    /// Returns a new descriptor (+1; the caller releases it) of a sampler described by `desc`, which the core accepted,
    /// labelled `label`: its filters, its address mode on every axis, its anisotropy, its compare function when
    /// `desc.compareEnable` is true, and argument-buffer support, because a shader reaches it through the sampler table. The
    /// caller holds an autorelease pool.
    MTL::SamplerDescriptor* samplerDescriptor(const SamplerDesc& desc, std::string_view label);

    /// Releases the `MTL::Texture` a view slot held: a view, or the texture itself retained. `context` is the `DeviceImpl`;
    /// a null payload is skipped. It runs under the view heap's lock.
    void retireView(void* context, void* native);

    /// Releases the `MTL::SamplerState` a sampler slot held. `context` is the `DeviceImpl`; a null payload is skipped. It
    /// runs under the sampler heap's lock.
    void retireSampler(void* context, void* native);
} // namespace rojoRHI::metal4
