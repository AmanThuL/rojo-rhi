//----------------------------------------------------------------------------------------------------------------------
/// @file RojoTrace.h
/// @brief Declares the tracing CommandList and Queue wrappers Null builds record through, and their call log.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rojoRHI::bench {
    // Appends a call's name to the log while tracing is on. The log holds names, never arguments.
    void traceCall(std::string_view name);
    // Clears the log and starts appending, with room for `capacity` names reserved.
    void startTrace(size_t capacity);
    void stopTrace();
    std::span<const std::string_view> tracedCalls();

    // One texture barrier record passed to a traced barrier call, with the call's position among the traced barrier
    // calls, counted from 0.
    struct TracedTextureBarrier
    {
        uint32_t call;
        TextureBarrier barrier;
    };

    // The texture barrier records of the traced barrier calls, in order; cleared by startTrace.
    std::span<const TracedTextureBarrier> tracedTextureBarriers();

    // A CommandList that logs each member's name, then forwards it. pushRoot logs uploadFrameData then setRoot, the
    // two calls RHI.h's template makes.
    class TracedList
    {
      public:
        TracedList() = default;
        explicit TracedList(CommandList list);

        CommandList native() const;

        void end();
        Addr uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);
        void beginRenderPass(const RenderPassDesc& desc);
        void endRenderPass();
        void beginScope(std::string_view label);
        void endScope();
        void setPipeline(Pipeline pipeline);
        void setViewport(const Viewport& viewport);
        void setScissor(const Rect2D& rect);
        void setStencilReference(uint8_t reference);
        void setBlendConstants(float r, float g, float b, float a);
        void setPrimitiveTopology(PrimitiveTopology topology);
        void setDepthBias(float constant, float slopeScaled, float clamp);
        void setRoot(Addr root);

        template<class T> Addr pushRoot(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "a root type is trivially copyable");
            static_assert(std::is_standard_layout_v<T>, "a root type is standard-layout");
            static_assert(sizeof(T) <= 65536, "a root block is at most 65536 bytes");
            const Addr address = uploadFrameData(std::as_bytes(std::span(&value, 1)), 256);
            setRoot(address);
            return address;
        }

        void draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
        void drawIndexed(
            Buffer indices, IndexType type, uint32_t count, uint32_t instances = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0
        );
        void drawMeshTasks(uint32_t x, uint32_t y, uint32_t z);
        void dispatch(uint32_t x, uint32_t y, uint32_t z);
        void drawIndirect(Addr args, uint32_t maxCount, Addr count = {});
        void drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count = {});
        void drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count = {});
        void dispatchIndirect(Addr args);
        void copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);
        void copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);
        void copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);
        void copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);
        void fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern);
        void clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value);
        void barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers = {}, std::span<const TextureBarrier> textures = {});
        void upscale(Upscaler upscaler, const UpscaleParams& params);
        void declareAccess(std::span<const ResourceAccess> accesses);

      private:
        CommandList m_list;
    };

    // A Queue that logs each member's name, then forwards it.
    class TracedQueue
    {
      public:
        TracedQueue() = default;
        explicit TracedQueue(Queue queue);

        void submit(std::span<const TracedList> lists, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {});
        void present(Swapchain swapchain);
        void mapTiles(std::span<const TileMapping> mappings);

      private:
        Queue m_queue;
    };
} // namespace rojoRHI::bench
