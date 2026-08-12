//----------------------------------------------------------------------------------------------------------------------
/// @file Texture.h
/// @brief Declares GPU textures, their descriptor, subresource vocabulary, and barrier uses.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Format.h"

#include <cstdint>
#include <string_view>

namespace lmx::rhi {

/// Identifies whether a texture is two-dimensional or a six-face cubemap.
enum class TextureKind {
    Tex2D, ///< A single two-dimensional image.
    Cube   ///< Six square faces sampled by direction.
};

/// Describes a GPU texture allocation and its allowed usages.
struct TextureDesc {
    uint32_t width = 0, height = 0;     ///< Extent in texels; cubemaps require equal dimensions.
    Format format = Format::BGRA8Unorm; ///< Pixel format.
    /// Cube requires width == height; width/height then describe one face, and the texture has
    /// six of them.
    TextureKind kind = TextureKind::Tex2D; ///< Texture dimensionality.
    /// 1..floor(log2(max(width, height))) + 1 -- the full chain down to a single texel. Levels
    /// beyond 0 are filled by the createTexture upload below; a caller with only level 0 in hand
    /// (Engine/Scene.cpp's unbaked-DDS fallback) passes a full mipLevels-sized span with the
    /// remaining entries null, leaving those levels' GPU content undefined until a future upload.
    uint32_t mipLevels = 1;    ///< Number of mip levels allocated for each face.
    bool renderTarget = false; ///< Enables render-target use.
    bool sampled = false;      ///< Enables shader reads.
    /// Enables shader reads through a storage binding. Distinct from `sampled` because a storage
    /// read is an unfiltered fetch through a read-write binding, which restricts the format.
    bool storageRead = false;
    bool storageWrite = false; ///< Enables shader writes through a storage binding.
    bool cpuReadback = false;  ///< Enables blocking CPU readback through shared storage.
    std::string_view label;    ///< Diagnostic object label.
};

/// mipLevelCount: every level from baseMipLevel to the end of the chain.
inline constexpr uint32_t kAllMipLevels = ~uint32_t{0};
/// arrayLayerCount: every layer from baseArrayLayer to the last one.
inline constexpr uint32_t kAllArrayLayers = ~uint32_t{0};

/// Names the mip levels and array layers of a texture that a view, binding, or barrier covers.
///
/// The default covers the whole resource, which is what every whole-resource declaration uses; a
/// narrower range is how a pass addresses one mip of a chain (a bloom step writing mip N+1 while
/// reading mip N), or one layer in a copy or barrier. Ranges are validated against the texture they
/// are used with: an empty range, or one running past the end of the chain, is a caller error.
struct TextureSubresourceRange {
    uint32_t baseMipLevel = 0;                  ///< First mip level in the range.
    uint32_t mipLevelCount = kAllMipLevels;     ///< Mip levels covered, or kAllMipLevels.
    uint32_t baseArrayLayer = 0;                ///< First array layer (cube face) in the range.
    uint32_t arrayLayerCount = kAllArrayLayers; ///< Layers covered, or kAllArrayLayers.
};

/// Describes the subresources and format a binding sees a texture through.
///
/// A default-constructed view is the whole texture in its own format, which is what a binding that
/// wants no reinterpretation passes. `format` reinterprets the same bits under a different transfer
/// function -- the sRGB and linear members of one format family -- and is validated against the
/// texture's family: a view may not reinterpret a format as one with a different bit layout.
struct TextureViewDesc {
    TextureSubresourceRange range; ///< Subresources the view exposes.
    /// Format the view reads and writes through, or Format::Unknown for the texture's own format.
    Format format = Format::Unknown;
};

/// Names the rectangle of one subresource that a copy command reads or writes.
///
/// A region addresses exactly one mip level of one array layer: a copy spanning several is several
/// copy calls, which is what keeps the addressing unambiguous. The origin and extent are in texels
/// of `mipLevel`, not of level zero, so copying mip 2 of a 64x64 chain uses a 16x16 extent.
///
/// `z` and `depth` complete the vocabulary a volume texture needs. This RHI models no 3D texture
/// kind, so until one exists `z` must be 0 and `depth` must be 1; they are stated here rather than
/// omitted so that adding the kind does not change the shape of every copy call.
///
/// The extent has no whole-mip default on purpose: a copy that silently resized itself to whatever
/// the texture happened to be is the kind of thing that works until the texture changes.
struct TextureCopyRegion {
    uint32_t mipLevel = 0;   ///< Mip level the region addresses.
    uint32_t arrayLayer = 0; ///< Array layer -- a cube face -- the region addresses.
    uint32_t x = 0;          ///< Origin x in texels of `mipLevel`.
    uint32_t y = 0;          ///< Origin y in texels of `mipLevel`.
    uint32_t z = 0;          ///< Origin z; must be 0 until a 3D texture kind exists.
    uint32_t width = 0;      ///< Extent width in texels of `mipLevel`.
    uint32_t height = 0;     ///< Extent height in texels of `mipLevel`.
    uint32_t depth = 1;      ///< Extent depth; must be 1 until a 3D texture kind exists.
};

/// Describes how a region's texels are laid out on the buffer side of a buffer<->texture copy.
///
/// `bytesPerRow` is the caller's for the same reason TextureMip::bytesPerRow is: only the caller
/// knows whether its rows are tightly packed or padded, and deriving a stride here would force a
/// caller holding a padded buffer to un-pad it first. It is a stride of the *region*, so a copy of
/// a 16x16 rectangle of RGBA8 texels is tightly packed at 64 bytes, whatever the texture's width.
///
/// `bytesPerSlice` is the distance between two array slices. Every copy this RHI can express covers
/// a single slice, so zero -- meaning "one slice, no image stride" -- is the value to pass; it is
/// named because a future array or volume copy has nowhere else to state it.
struct BufferTextureLayout {
    uint64_t offset = 0;        ///< First byte of the region's texels within the buffer.
    uint64_t bytesPerRow = 0;   ///< Distance in bytes between the starts of two rows.
    uint64_t bytesPerSlice = 0; ///< Distance in bytes between two slices, or 0 for a single slice.
};

/// Describes the CPU upload payload for one mip level of one texture face.
///
/// bytesPerRow is the caller's because only the caller knows the source layout: for an
/// uncompressed format it is width * bytesPerTexel, but for BC1 a "row" is a row of 4x4 *blocks*
/// -- ((width + 3) / 4) * 8 -- and rows 1..3 of every block live inside that same stride. Note the
/// rounding is *up*: a width of 6 is two blocks (16 bytes), not one, because the sixth column
/// still needs a block to live in. Truncating division agrees with the ceiling on every power of
/// two and disagrees on everything else, which is a stride bug that fails silently.
///
/// The RHI deliberately does not derive any of this: a decoder that hands over a padded or
/// block-aligned buffer would then have to un-pad it first.
///
/// data == nullptr leaves that level untouched -- undefined GPU content until something else
/// uploads it. A caller with only level 0 in hand still passes a full mipLevels * faceCount span,
/// with the remaining entries left null.
struct TextureMip {
    const void* data = nullptr; ///< Source bytes, or null to leave the level untouched.
    uint64_t bytesPerRow = 0;   ///< Source row stride in bytes.
};

/// Provides dimensions and optional CPU readback for a GPU texture.
class Texture {
public:
    /// Destroys the texture after its owning device has finished using it.
    virtual ~Texture() = default;
    /// Returns the texture width in texels.
    virtual uint32_t width() const = 0;
    /// Returns the texture height in texels.
    virtual uint32_t height() const = 0;
    /// Returns the pixel format the texture was created with.
    virtual Format format() const = 0;
    /// Returns the number of mip levels each face was allocated with.
    virtual uint32_t mipLevels() const = 0;
    /// Returns the number of array layers: one for a 2D texture, six for a cubemap's faces.
    virtual uint32_t arrayLayers() const = 0;
    /// Blocking readback of the full texture (requires cpuReadback). out must hold exactly
    /// width * height * bytesPerPixel(format) bytes, tightly packed, in the format's own channel
    /// order -- see bytesPerPixel in RHI/Validate.h, which also decides which formats readback
    /// accepts at all. Caller ensures GPU work completed (Device::waitIdle).
    /// Copies the full tightly packed texture into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// How a texture is being used at a barrier boundary. Grows per real feature demand, exactly
/// like the rest of this header (ADR 0004).
/// Identifies texture use on either side of an explicit barrier.
enum class TextureUse {
    RenderTarget,   ///< Written as a render-pass attachment.
    ShaderRead,     ///< Read by a shader.
    StorageRead,    ///< Read through a storage binding.
    StorageWrite,   ///< Written through a storage binding.
    CopySource,     ///< Read by a copy command.
    CopyDestination ///< Written by a copy command.
};

} // namespace lmx::rhi
