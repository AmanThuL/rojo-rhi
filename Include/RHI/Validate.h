//----------------------------------------------------------------------------------------------------------------------
/// @file Validate.h
/// @brief Declares backend-neutral validation helpers for RHI descriptors.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/ComputePipeline.h"
#include "RHI/Format.h"
#include "RHI/GraphicsPipeline.h"
#include "RHI/Heap.h"
#include "RHI/RenderPass.h"
#include "RHI/Result.h"
#include "RHI/Sampler.h"
#include "RHI/Swapchain.h"
#include "RHI/Texture.h"

#include <cstdint>

namespace lmx::rhi {

/// Validates a buffer descriptor before backend object creation.
Result<void> validate(const BufferDesc& desc);
/// Validates a texture descriptor before backend object creation.
Result<void> validate(const TextureDesc& desc);
/// Validates a sampler descriptor before backend object creation.
Result<void> validate(const SamplerDesc& desc);
/// Validates a graphics pipeline descriptor before backend object creation.
Result<void> validate(const GraphicsPipelineDesc& desc);
/// Validates a compute pipeline descriptor before backend object creation.
Result<void> validate(const ComputePipelineDesc& desc);
/// Validates a swapchain descriptor before backend object creation.
Result<void> validate(const SwapchainDesc& desc);
/// Validates a heap descriptor before backend object creation.
Result<void> validate(const HeapDesc& desc);

/// Validates one placement: that `footprint` is a real size and alignment, that `offset` satisfies
/// the alignment, and that the whole footprint fits inside `heap`.
Result<void> validatePlacement(const Heap& heap, uint64_t offset, const SizeAlign& footprint);

/// Validates the attachment combination for a render pass.
Result<void> validateRenderPassTargets(const Texture* color, const Texture* depth);

/// Validates a render pass's extra color attachments: the count against kMaxExtraColorTargets,
/// their dependence on a primary `color` attachment, and each target's presence, color-renderable
/// format, and agreement with the primary's extent.
Result<void> validateExtraColorTargets(const Texture* color, const ExtraColorTarget* extraColor,
                                       uint32_t extraColorCount);

/// Validates a render pass's origin-anchored render area against the extent it draws into: the
/// primary color attachment's, or a depth-only pass's depth attachment. A zero pair -- the whole
/// attachment -- is always accepted; a half-set pair never is.
Result<void> validateRenderArea(const Texture* color, const Texture* depth, uint32_t width,
                                uint32_t height);

/// Validates a subresource range against the texture it addresses, resolving the kAllMipLevels and
/// kAllArrayLayers sentinels against that texture's own extents.
Result<void> validateSubresourceRange(const Texture& texture, const TextureSubresourceRange& range);

/// Validates a texture view: its range against the texture, and its format against the texture's
/// format family.
Result<void> validateTextureView(const Texture& texture, const TextureViewDesc& view);

/// Validates a barrier's byte range against the buffer it addresses, resolving the kWholeBuffer
/// sentinel against that buffer's own size.
Result<void> validateBufferRange(const Buffer& buffer, const BufferRange& range);

/// Validates an explicit byte range -- one with no whole-buffer sentinel, as fillBuffer takes --
/// against the buffer it addresses.
Result<void> validateBufferBytes(const Buffer& buffer, uint64_t offset, uint64_t size);

/// Validates a buffer-to-buffer copy: both ranges against their allocations, and the two against
/// each other when they name the same buffer.
Result<void> validateBufferCopy(const Buffer& source, uint64_t sourceOffset,
                                const Buffer& destination, uint64_t destinationOffset,
                                uint64_t size);

/// Validates a copy region against the texture and the mip level it addresses.
Result<void> validateTextureCopyRegion(const Texture& texture, const TextureCopyRegion& region);

/// Validates a buffer<->texture copy: the region against the texture, the layout's strides against
/// the region and the format's texel size, and the addressed bytes against the buffer.
Result<void> validateBufferTextureCopy(const Buffer& buffer, const BufferTextureLayout& layout,
                                       const Texture& texture, const TextureCopyRegion& region);

/// Validates a texture-to-texture copy: both regions, their matching extents and formats, and
/// their disjointness when both name the same texture.
Result<void> validateTextureCopy(const Texture& source, const TextureCopyRegion& sourceRegion,
                                 const Texture& destination,
                                 const TextureCopyRegion& destinationRegion);

/// Validates that `argsSize` bytes of indirect arguments sit at an aligned, in-bounds offset.
Result<void> validateIndirectArgs(const Buffer& buffer, uint64_t offset, uint64_t argsSize);

/// Validates one CommandList::bindFrameData request: that `slot` names a buffer binding, that the
/// block is a real one, and that `alignment` is a power of two of at least kFrameDataAlignment.
///
/// It deliberately says nothing about capacity. How much frame-owned memory exists is a backend's
/// own growable resource rather than a limit the caller is asked to respect, so exhausting it is
/// reported by the backend as device-resource failure and never surfaces as an invalid request.
Result<void> validateFrameData(uint32_t slot, const void* data, uint64_t size, uint64_t alignment);

/// Returns the extent of a mip level of a texture whose level-zero extent is `base`, floored at one
/// texel exactly as the hardware's chain is.
uint32_t mipExtent(uint32_t base, uint32_t level);

/// Returns true for formats a storage binding can read or write. Storage access is an unfiltered
/// read-write fetch, which the hardware supports for far fewer formats than sampling does.
bool isStorageFormat(Format format);

/// Returns the tightly packed byte size of a readable texel, or zero for unsupported formats.
uint32_t bytesPerPixel(Format format);

} // namespace lmx::rhi
