//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleKit.h
/// @brief Declares the helper that builds an upscaler and a valid set of upscale arguments on a recording list.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Support/PassKit.h"

#include <cstdint>

namespace rojoRHI::test {
    /// A `PassKit` device with an open frame and an acquired list, an upscaler and the arguments of a valid upscale on it. It
    /// never ends or destroys anything, so a death test can abort at any point.
    class UpscaleKit
    {
      public:
        /// The input extent of `defaultDesc`, in texels.
        static constexpr uint32_t kInput = 64;
        /// The output extent of `defaultDesc`, in texels.
        static constexpr uint32_t kOutput = 128;

        /// Returns the description of the upscaler of `kInput` to `kOutput` texels the kit builds by default.
        static UpscalerDesc defaultDesc();

        /// Creates the device and list of `type`, an upscaler of `desc`, and textures that make `params` a valid upscale of it.
        /// `validation` selects `DeviceDesc::validation`, which a Release build refuses.
        explicit UpscaleKit(QueueType type = QueueType::Graphics, const UpscalerDesc& desc = defaultDesc(), bool validation = false);

        /// Creates a 2D texture of `format`, `extent` and `usage` on the kit's device.
        Texture make(Format format, Extent2D extent, TextureUsage usage);

        /// Creates a 1x1 `R16Float` sampled texture, a valid exposure.
        Texture exposureTexture();

        /// Creates a sampled `R8Unorm` texture of the descriptor's input extent, a valid reactive mask.
        Texture reactiveTexture();

        /// Acquires an image of a new swapchain on the kit's device, for the tests of the entries that refuse one.
        Texture swapchainImage();

        /// Records `params` on `list` for `upscaler`.
        void upscale();

        /// Ends the list, submits it, ends the frame and opens the next one with a new list of the same type.
        void nextFrame();

        PassKit kit;          ///< The device, the open frame and the list.
        UpscalerDesc desc;    ///< The description of `upscaler`.
        Upscaler upscaler;    ///< The upscaler.
        UpscaleParams params; ///< A valid set of arguments for `upscaler`.

      private:
        /// Records on `list`, when the kit moves what it creates, a barrier that moves `texture` from `Undefined` to `layout`,
        /// and returns `texture`.
        Texture moved(Texture texture, Layout layout);

        /// The kit's valid inputs and output move out of `Undefined` for the upscale: on a Graphics list without validation,
        /// where the backend's layouts are native (`layoutsAreNative`), as `PassKit` moves its targets.
        bool m_moveTextures = false;
    };
} // namespace rojoRHI::test
