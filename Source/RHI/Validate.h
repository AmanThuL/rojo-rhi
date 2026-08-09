#pragma once
#include "RHI/RHI.h"

namespace lmx::rhi {

// Pure precondition checks on the creation descs. A backend calls the matching
// overload before touching the GPU API, so a malformed desc fails with a message
// naming the offending field instead of an opaque driver error much later.
// Every failure carries ErrorCode::InvalidDesc.
Result<void> validate(const BufferDesc& desc);
Result<void> validate(const TextureDesc& desc);
Result<void> validate(const SamplerDesc& desc);
Result<void> validate(const GraphicsPipelineDesc& desc);
Result<void> validate(const SwapchainDesc& desc);

Result<void> validateRenderPassTargets(const Texture* color, const Texture* depth);

// Bytes one texel of `format` occupies in the tightly packed image Texture::readback produces,
// and 0 for every format this RHI does not read back: block-compressed formats, which have no
// per-texel size at all, and formats no caller has needed a readback of yet. TextureDesc
// validation and the backend's readback path share it, so a format becomes readback-capable --
// and gains its buffer-size contract -- in exactly one place.
uint32_t bytesPerPixel(Format format);

} // namespace lmx::rhi
