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

        /// Creates the device, opens a frame and acquires a list of `type`. `validation` selects `DeviceDesc::validation`, which a
        /// Release build refuses, so only Debug-only tests pass true.
        explicit PassKit(QueueType type = QueueType::Graphics, bool validation = false);

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

        /// Creates a graphics pipeline over the color `targets` (so a test can blend) and `depth`, with the depth-stencil state
        /// `depthStencil` (so a test can write depth or stencil).
        Pipeline graphicsWith(std::span<const ColorTarget> targets, Format depth, const DepthStencilState& depthStencil, uint32_t samples = 1);

        /// Creates a compute pipeline.
        Pipeline compute();

        /// Begins a pass over one `RGBA8Unorm` color attachment of the whole texture. A validating kit first records the barrier
        /// that moves the texture to `ColorAttachment`.
        void beginSimplePass();

        /// Creates a buffer of `size` bytes with `usage` in `memory`.
        Buffer buffer(BufferUsage usage, uint64_t size = 4096, Memory memory = Memory::Gpu);

        /// Creates a `kSize` square 2D texture, an array texture when `layers` is above one, with `usage`.
        Texture texture(Format format = Format::RGBA8Unorm, TextureUsage usage = TextureUsage::Sampled, uint32_t mips = 1, uint32_t layers = 1);

        /// Begins a simple pass, binds a vertex pipeline (a mesh pipeline when `mesh` is set) and sets a viewport and scissor.
        void readyToDraw(bool mesh = false);

        /// Ends the list, submits it to the queue of its type, ends the frame and destroys the device.
        void finish();

        Device device;         ///< The device.
        ShaderLibrary library; ///< The standard library.
        CommandList list;      ///< The acquired list.
        QueueType type;        ///< The queue type of `list`.

      private:
        bool m_validation; ///< The device was created with validation.
    };

    /// Returns a pass description of the color attachments `colors` that renders to the whole `PassKit::kSize` square.
    RenderPassDesc fullPass(std::span<const ColorAttachment> colors);
} // namespace rojoRHI::test
