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

} // namespace lmx::rhi
