//----------------------------------------------------------------------------------------------------------------------
/// @file Validate.h
/// @brief Declares backend-neutral validation helpers for RHI descriptors.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/RHI.h"

namespace lmx::rhi {

/// Validates a buffer descriptor before backend object creation.
Result<void> validate(const BufferDesc& desc);
/// Validates a texture descriptor before backend object creation.
Result<void> validate(const TextureDesc& desc);
/// Validates a sampler descriptor before backend object creation.
Result<void> validate(const SamplerDesc& desc);
/// Validates a graphics pipeline descriptor before backend object creation.
Result<void> validate(const GraphicsPipelineDesc& desc);
/// Validates a swapchain descriptor before backend object creation.
Result<void> validate(const SwapchainDesc& desc);

/// Validates the attachment combination for a render pass.
Result<void> validateRenderPassTargets(const Texture* color, const Texture* depth);

/// Returns the tightly packed byte size of a readable texel, or zero for unsupported formats.
uint32_t bytesPerPixel(Format format);

} // namespace lmx::rhi
