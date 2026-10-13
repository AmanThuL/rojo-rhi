//----------------------------------------------------------------------------------------------------------------------
/// @file PassKit.h
/// @brief Declares the helper that builds a device, a recording list, attachment textures and pipelines for the render pass tests.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>
#include <span>

namespace rojoRHI::test {
    /// A device with an open frame, a library of the standard entries and one acquired list, with factories for the objects a
    /// render pass names. It never ends or destroys anything, so a death test can abort at any point.
    class PassKit
    {
      public:
        /// The edge of the textures `target` and `depthTarget` create, in texels.
        static constexpr uint32_t kSize = 64;

        /// Creates the device, opens a frame and acquires a list of `type`.
        explicit PassKit(QueueType type = QueueType::Graphics);

        /// Creates a `kSize` square `RenderTarget` texture.
        Texture target(Format format = Format::RGBA8Unorm, uint32_t samples = 1, uint32_t mips = 1, uint32_t layers = 1);

        /// Creates a `kSize` square `DepthStencil` texture.
        Texture depthTarget(Format format = Format::D32Float, uint32_t samples = 1);

        /// Creates a graphics pipeline over `colors`, `depth` and `samples`; a mesh pipeline when `mesh` is set.
        Pipeline graphics(
            std::span<const Format> colors,
            Format depth = Format::Undefined,
            uint32_t samples = 1,
            PrimitiveClass primitive = PrimitiveClass::Triangle,
            bool depthBias = false,
            bool mesh = false
        );

        /// Creates a compute pipeline.
        Pipeline compute();

        /// Begins a pass over one `RGBA8Unorm` color attachment of the whole texture.
        void beginSimplePass();

        /// Creates a `Gpu` buffer of `size` bytes with `usage`.
        Buffer buffer(BufferUsage usage, uint64_t size = 4096);

        /// Begins a simple pass, binds a vertex pipeline (a mesh pipeline when `mesh` is set) and sets a viewport and scissor.
        void readyToDraw(bool mesh = false);

        /// Ends the list, submits it to the queue of its type, ends the frame and destroys the device.
        void finish();

        Device device;         ///< The device.
        ShaderLibrary library; ///< The standard library.
        CommandList list;      ///< The acquired list.
        QueueType type;        ///< The queue type of `list`.
    };

    /// Returns a pass description of the color attachments `colors` that renders to the whole `PassKit::kSize` square.
    RenderPassDesc fullPass(std::span<const ColorAttachment> colors);
} // namespace rojoRHI::test
