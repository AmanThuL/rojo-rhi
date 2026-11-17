//----------------------------------------------------------------------------------------------------------------------
/// @file PassKit.h
/// @brief Declares the helper that builds a device, a recording list, attachment textures and pipelines for the render pass tests.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstdint>
#include <span>

namespace rojoRHI::test {
    // The library of the standard entries loads when the first pipeline needs it, so a kit that only copies or clears never
    // loads one. It never ends or destroys anything, so a death test can abort at any point.
    class PassKit
    {
      public:
        static constexpr uint32_t kSize = 64;

        // `validation` selects `DeviceDesc::validation`, which a Release build refuses, so only Debug-only tests pass true.
        explicit PassKit(QueueType type = QueueType::Graphics, bool validation = false, bool scopeTimings = false, bool debugMarkers = false);

        // Where layouts are native and the kit does not validate, a Graphics kit first records the barrier that moves it to
        // `ColorAttachment`, which a pass needs and the case does not record.
        Texture target(Format format = Format::RGBA8Unorm, uint32_t samples = 1, uint32_t mips = 1, uint32_t layers = 1);

        // Where layouts are native and the kit does not validate, a Graphics kit first records the barrier that moves it to
        // `DepthStencilWrite`.
        Texture depthTarget(Format format = Format::D32Float, uint32_t samples = 1);

        Pipeline graphics(
            std::span<const Format> colors,
            Format depth = Format::Undefined,
            uint32_t samples = 1,
            PrimitiveClass primitive = PrimitiveClass::Triangle,
            bool depthBias = false,
            bool mesh = false
        );

        Pipeline graphicsWith(std::span<const ColorTarget> targets, Format depth, const DepthStencilState& depthStencil, uint32_t samples = 1);

        Pipeline compute();

        // A validating kit first records the barrier that moves the texture to `ColorAttachment`. Where layouts are native and
        // the kit does not validate, every call renders to one texture `target` moved once, so a call inside an open pass
        // records no barrier there.
        void beginSimplePass();

        Buffer buffer(BufferUsage usage, uint64_t size = 4096, Memory memory = Memory::Gpu);

        Texture texture(Format format = Format::RGBA8Unorm, TextureUsage usage = TextureUsage::Sampled, uint32_t mips = 1, uint32_t layers = 1);

        void readyToDraw(bool mesh = false);

        // Each of the compute and copy queues signals a fence value from a submission with no lists, which a graphics
        // submission with no lists then waits on, as VU-rojo-device-0006 requires of a frame.
        void joinQueues();

        // A validating kit's queues are joined before the frame ends and the device is destroyed.
        void finish();

        Device device;
        ShaderLibrary library; // The standard library; null until the first pipeline is created, then kept for the kit's life.
        CommandList list;
        QueueType type;

      private:
        ShaderLibrary ensureLibrary();

        bool m_validation;
        bool m_moveTargets;     //`target` and `depthTarget` move what they create out of `Undefined` (`layoutsAreNative`).
        Texture m_simpleTarget; // The texture `beginSimplePass` renders to while `m_moveTargets`; null until its first call.
        Fence m_join;           // The fence `joinQueues` signals and waits on; null until its first call.
        uint64_t m_joined = 0;
    };

    RenderPassDesc fullPass(std::span<const ColorAttachment> colors);

    // Chapter 09's table, read by layout.
    Access accessesOf(Layout layout);

    // Every stage on both sides, the write accesses `oldLayout` permits as the source and every access `newLayout` permits
    // as the destination, as a caller ordering any use before it and any use after it would. A discard from `Undefined`
    // names no source access, as VU-rojo-sync-0016 requires.
    void transition(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, const SubresourceRange& range = {});
} // namespace rojoRHI::test
