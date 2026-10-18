//----------------------------------------------------------------------------------------------------------------------
/// @file FrameRoots.h
/// @brief Declares the root block of each pass kind of the composed frame, flat per the shader ABI and mirrored in Slang.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/Scene.h"

#include <rojoRHI/RHI.h>

#include <cstdint>

namespace rojoRHI::bench {
    // Every root is 32-bit scalars, two- and four-component vectors and Addr, each at a multiple of its alignment, with
    // explicit padding and a size that is a multiple of 16. View and sampler indices are filled by each arm with its own.
    // The Slang mirror of each is checked by Shaders/Bench/BenchFrameProbe.slang at build time.

    // The culling kernel: the instances it reads and, per view, the array it writes records into and the count it writes.
    struct alignas(16) CullRoot
    {
        Addr instances;
        Addr records0;
        Addr records1;
        Addr records2;
        Addr records3;
        Addr counts0;
        Addr counts1;
        Addr counts2;
        Addr counts3;
        uint32_t pad0;
        uint32_t pad1;
    };

    // The geometry pass: the instances and mesh vertices it reads and the camera's view-projection rows.
    struct alignas(16) GeometryRoot
    {
        Addr instances;
        Addr vertices;
        Float4 viewProj0;
        Float4 viewProj1;
        Float4 viewProj2;
        Float4 viewProj3;
    };

    // A shadow pass: as the geometry pass, with its cascade's view-projection rows.
    struct alignas(16) ShadowRoot
    {
        Addr instances;
        Addr vertices;
        Float4 viewProj0;
        Float4 viewProj1;
        Float4 viewProj2;
        Float4 viewProj3;
    };

    // The occlusion kernel: the `depth` and `normal` views it reads, the `aoRaw` view it writes, and the image size.
    struct alignas(16) AoRoot
    {
        uint32_t depth;
        uint32_t normal;
        uint32_t aoRaw;
        uint32_t width;
        uint32_t height;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
    };

    // The blur kernel: the `aoRaw` view it reads, the `ao` view it writes, and the image size.
    struct alignas(16) BlurRoot
    {
        uint32_t aoRaw;
        uint32_t ao;
        uint32_t width;
        uint32_t height;
    };

    // The lighting kernel: the G-buffer, `shadow` (a three-layer array view), `ao` and `hdr` views, the image size, the
    // light direction, the inverse camera view-projection and the three cascades' view-projections.
    struct alignas(16) LightingRoot
    {
        uint32_t albedo;
        uint32_t normal;
        uint32_t material;
        uint32_t depth;
        uint32_t shadow;
        uint32_t ao;
        uint32_t hdr;
        uint32_t width;
        uint32_t height;
        uint32_t pad0;
        uint32_t pad1;
        uint32_t pad2;
        Float4 lightDirection;
        Float4 inverseViewProj0;
        Float4 inverseViewProj1;
        Float4 inverseViewProj2;
        Float4 inverseViewProj3;
        Float4 cascade0Row0;
        Float4 cascade0Row1;
        Float4 cascade0Row2;
        Float4 cascade0Row3;
        Float4 cascade1Row0;
        Float4 cascade1Row1;
        Float4 cascade1Row2;
        Float4 cascade1Row3;
        Float4 cascade2Row0;
        Float4 cascade2Row1;
        Float4 cascade2Row2;
        Float4 cascade2Row3;
    };

    // One forward box: its transform rows and color, the mesh vertices it reads and `camera`, the address of the camera's
    // view-projection (64 bytes, one Transform) that the lighting list uploads once before the forward pass.
    struct alignas(16) ForwardRoot
    {
        Float4 transform0;
        Float4 transform1;
        Float4 transform2;
        Float4 transform3;
        Float4 color;
        Addr vertices;
        Addr camera;
    };

    // A bloom downsample: the source view, the linear sampler, and the source texel size.
    struct alignas(16) BloomDownRoot
    {
        uint32_t source;
        uint32_t samplerId;
        float texelX;
        float texelY;
    };

    // A bloom upsample: the source view, the linear sampler and the intensity added to the target.
    struct alignas(16) BloomUpRoot
    {
        uint32_t source;
        uint32_t samplerId;
        float intensity;
        uint32_t pad0;
    };

    // The tone map: the `hdr` view, the linear sampler and the exposure.
    struct alignas(16) ToneMapRoot
    {
        uint32_t source;
        uint32_t samplerId;
        float exposure;
        uint32_t pad0;
    };

    inline constexpr uint32_t kRootStructCount = 10;
} // namespace rojoRHI::bench
