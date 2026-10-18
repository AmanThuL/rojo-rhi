//----------------------------------------------------------------------------------------------------------------------
/// @file RojoFrameResources.h
/// @brief Declares the composed frame's RojoRHI objects: buffers, textures, views, the sampler and the pipelines.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/FrameLayout.h"
#include "Rojo/RojoContext.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>

namespace rojoRHI::bench {
    // Every object the composed frame records with, created through the context, which owns and destroys them. The
    // buffers and textures are FrameLayout's, with its labels; the scene buffers hold their bytes from creation.
    struct RojoFrameResources
    {
        Buffer instances;
        Buffer vertices;
        Buffer indices;
        std::array<Buffer, kFrameSlots> records;
        std::array<Buffer, kFrameSlots> readback;
        std::array<Texture, kTextureCount> textures;

        // A Sampled view of every texture created Sampled (all but `ldr`): Tex2D, or Tex2DArray over the three layers
        // of `shadow`, with the depth aspect for `depth` and `shadow`. Storage views of the three textures kernels write.
        std::array<ViewIndex, kTextureCount> sampled{};
        ViewIndex aoRawStorage{};
        ViewIndex aoStorage{};
        ViewIndex hdrStorage{};
        SamplerIndex sampler{};

        // One pipeline per kind of pass: the three cascades share one, as do the two bloom passes of each direction.
        Pipeline cull;
        Pipeline geometry;
        Pipeline shadow;
        Pipeline ao;
        Pipeline blur;
        Pipeline lighting;
        Pipeline forward;
        Pipeline bloomDown;
        Pipeline bloomUp;
        Pipeline toneMap;

        Texture texture(FrameTexture which) const;
        uint32_t view(FrameTexture which) const; ///< The Sampled view, as a root holds it.
    };

    /// The render pass of kRenderPasses that records pass `pass` of kPasses; every caller names a render pass.
    const RenderPassSpec& renderPassSpec(uint32_t pass);

    // Creates every object of RojoFrameResources, loading the frame's libraries through loadLibrary with the entries
    // each pipeline names. A failure names the object.
    std::expected<RojoFrameResources, std::string> createFrameResources(RojoContext& context);
} // namespace rojoRHI::bench
