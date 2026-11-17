//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleKit.h
/// @brief Declares the helper that builds an upscaler and a valid set of upscale arguments on a recording list.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <cstdint>

namespace rojoRHI::test {
    // It never ends or destroys anything, so a death test can abort at any point.
    class UpscaleKit
    {
      public:
        static constexpr uint32_t kInput = 64;
        static constexpr uint32_t kOutput = 128;

        static UpscalerDesc defaultDesc();

        // `validation` selects `DeviceDesc::validation`, which a Release build refuses.
        explicit UpscaleKit(
            QueueType type = QueueType::Graphics,
            const UpscalerDesc& desc = defaultDesc(),
            bool validation = false,
            bool scopeTimings = false,
            bool debugMarkers = false
        );

        Texture make(Format format, Extent2D extent, TextureUsage usage);

        Texture exposureTexture();

        Texture reactiveTexture();

        // For the tests of the entries that refuse a swapchain image.
        Texture swapchainImage();

        void upscale();

        void nextFrame();

        PassKit kit;
        UpscalerDesc desc;
        Upscaler upscaler;
        UpscaleParams params;

      private:
        // Records the barrier only when the kit moves what it creates.
        Texture moved(Texture texture, Layout layout);

        // The kit's valid inputs and output move out of `Undefined` for the upscale on a Graphics list without validation,
        // where the backend's layouts are native (`layoutsAreNative`), as `PassKit` moves its targets.
        bool m_moveTextures = false;
    };
} // namespace rojoRHI::test
