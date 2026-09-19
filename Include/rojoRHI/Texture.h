//----------------------------------------------------------------------------------------------------------------------
/// @file Texture.h
/// @brief Declares GPU texture interfaces and barrier uses.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <rojoRHI/TextureDesc.h>

#include <cstdint>

namespace rojoRHI {

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
    /// order -- see bytesPerPixel in rojoRHI/Validate.h, which also decides which formats readback
    /// accepts at all. Caller ensures GPU work completed (Device::waitIdle).
    /// Copies the full tightly packed texture into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// How a texture is being used at a barrier boundary. Grows per real feature demand, exactly
/// like the rest of this header (ADR 0004).
/// Identifies texture use on either side of an explicit barrier.
enum class TextureUse {
    RenderTarget,    ///< Written as a render-pass attachment.
    ShaderRead,      ///< Read by a shader.
    StorageRead,     ///< Read through a storage binding.
    StorageWrite,    ///< Written through a storage binding.
    CopySource,      ///< Read by a copy command.
    CopyDestination, ///< Written by a copy command.
    ExternalRead,    ///< Read by an opaque RHI operation.
    ExternalWrite    ///< Written by an opaque RHI operation.
};

} // namespace rojoRHI
