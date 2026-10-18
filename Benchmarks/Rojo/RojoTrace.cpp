//----------------------------------------------------------------------------------------------------------------------
/// @file RojoTrace.cpp
/// @brief Implements the call log and the forwarding members of the tracing wrappers.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoTrace.h"

#include <array>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        // The frame thread is the only writer; only Null builds, which are never timed, record through the wrappers.
        std::vector<std::string_view> g_calls;
        bool g_tracing = false;
        // Lists per submission unwrapped without an allocation.
        constexpr size_t kStackLists = 16;
    } // namespace

    //==================================================================================================================
    void traceCall(std::string_view name)
    {
        if (g_tracing)
        {
            g_calls.push_back(name);
        }
    }

    //==================================================================================================================
    void startTrace(size_t capacity)
    {
        g_calls.clear();
        g_calls.reserve(capacity);
        g_tracing = true;
    }

    //==================================================================================================================
    void stopTrace()
    {
        g_tracing = false;
    }

    //==================================================================================================================
    std::span<const std::string_view> tracedCalls()
    {
        return g_calls;
    }

    //==================================================================================================================
    TracedList::TracedList(CommandList list) : m_list(list)
    {
    }

    //==================================================================================================================
    CommandList TracedList::native() const
    {
        return m_list;
    }

    //==================================================================================================================
    void TracedList::end()
    {
        traceCall("end");
        m_list.end();
    }

    //==================================================================================================================
    Addr TracedList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment)
    {
        traceCall("uploadFrameData");
        return m_list.uploadFrameData(bytes, alignment);
    }

    //==================================================================================================================
    void TracedList::beginRenderPass(const RenderPassDesc& desc)
    {
        traceCall("beginRenderPass");
        m_list.beginRenderPass(desc);
    }

    //==================================================================================================================
    void TracedList::endRenderPass()
    {
        traceCall("endRenderPass");
        m_list.endRenderPass();
    }

    //==================================================================================================================
    void TracedList::beginScope(std::string_view label)
    {
        traceCall("beginScope");
        m_list.beginScope(label);
    }

    //==================================================================================================================
    void TracedList::endScope()
    {
        traceCall("endScope");
        m_list.endScope();
    }

    //==================================================================================================================
    void TracedList::setPipeline(Pipeline pipeline)
    {
        traceCall("setPipeline");
        m_list.setPipeline(pipeline);
    }

    //==================================================================================================================
    void TracedList::setViewport(const Viewport& viewport)
    {
        traceCall("setViewport");
        m_list.setViewport(viewport);
    }

    //==================================================================================================================
    void TracedList::setScissor(const Rect2D& rect)
    {
        traceCall("setScissor");
        m_list.setScissor(rect);
    }

    //==================================================================================================================
    void TracedList::setStencilReference(uint8_t reference)
    {
        traceCall("setStencilReference");
        m_list.setStencilReference(reference);
    }

    //==================================================================================================================
    void TracedList::setBlendConstants(float r, float g, float b, float a)
    {
        traceCall("setBlendConstants");
        m_list.setBlendConstants(r, g, b, a);
    }

    //==================================================================================================================
    void TracedList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        traceCall("setPrimitiveTopology");
        m_list.setPrimitiveTopology(topology);
    }

    //==================================================================================================================
    void TracedList::setDepthBias(float constant, float slopeScaled, float clamp)
    {
        traceCall("setDepthBias");
        m_list.setDepthBias(constant, slopeScaled, clamp);
    }

    //==================================================================================================================
    void TracedList::setRoot(Addr root)
    {
        traceCall("setRoot");
        m_list.setRoot(root);
    }

    //==================================================================================================================
    void TracedList::draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance)
    {
        traceCall("draw");
        m_list.draw(vertices, instances, firstVertex, firstInstance);
    }

    //==================================================================================================================
    void TracedList::drawIndexed(
        Buffer indices, IndexType type, uint32_t count, uint32_t instances, uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance
    )
    {
        traceCall("drawIndexed");
        m_list.drawIndexed(indices, type, count, instances, firstIndex, baseVertex, firstInstance);
    }

    //==================================================================================================================
    void TracedList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        traceCall("drawMeshTasks");
        m_list.drawMeshTasks(x, y, z);
    }

    //==================================================================================================================
    void TracedList::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        traceCall("dispatch");
        m_list.dispatch(x, y, z);
    }

    //==================================================================================================================
    void TracedList::drawIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        traceCall("drawIndirect");
        m_list.drawIndirect(args, maxCount, count);
    }

    //==================================================================================================================
    void TracedList::drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count)
    {
        traceCall("drawIndexedIndirect");
        m_list.drawIndexedIndirect(indices, type, args, maxCount, count);
    }

    //==================================================================================================================
    void TracedList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        traceCall("drawMeshTasksIndirect");
        m_list.drawMeshTasksIndirect(args, maxCount, count);
    }

    //==================================================================================================================
    void TracedList::dispatchIndirect(Addr args)
    {
        traceCall("dispatchIndirect");
        m_list.dispatchIndirect(args);
    }

    //==================================================================================================================
    void TracedList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size)
    {
        traceCall("copyBuffer");
        m_list.copyBuffer(src, srcOffset, dst, dstOffset, size);
    }

    //==================================================================================================================
    void TracedList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region)
    {
        traceCall("copyBufferToTexture");
        m_list.copyBufferToTexture(src, layout, dst, region);
    }

    //==================================================================================================================
    void TracedList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout)
    {
        traceCall("copyTextureToBuffer");
        m_list.copyTextureToBuffer(src, region, dst, layout);
    }

    //==================================================================================================================
    void TracedList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion)
    {
        traceCall("copyTexture");
        m_list.copyTexture(src, srcRegion, dst, dstRegion);
    }

    //==================================================================================================================
    void TracedList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern)
    {
        traceCall("fillBuffer");
        m_list.fillBuffer(dst, offset, size, pattern);
    }

    //==================================================================================================================
    void TracedList::clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value)
    {
        traceCall("clearTexture");
        m_list.clearTexture(texture, range, value);
    }

    //==================================================================================================================
    void TracedList::barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures)
    {
        traceCall("barrier");
        m_list.barrier(globals, buffers, textures);
    }

    //==================================================================================================================
    void TracedList::upscale(Upscaler upscaler, const UpscaleParams& params)
    {
        traceCall("upscale");
        m_list.upscale(upscaler, params);
    }

    //==================================================================================================================
    void TracedList::declareAccess(std::span<const ResourceAccess> accesses)
    {
        traceCall("declareAccess");
        m_list.declareAccess(accesses);
    }

    //==================================================================================================================
    TracedQueue::TracedQueue(Queue queue) : m_queue(queue)
    {
    }

    //==================================================================================================================
    void TracedQueue::submit(std::span<const TracedList> lists, std::span<const FenceValue> wait, std::span<const FenceValue> signal)
    {
        traceCall("submit");
        // The lists unwrapped on the stack, so a submission allocates no more on Null than RojoRHI itself does.
        std::array<CommandList, kStackLists> stack;
        std::vector<CommandList> heap;
        std::span<CommandList> natives(stack.data(), lists.size());
        if (lists.size() > kStackLists)
        {
            heap.resize(lists.size());
            natives = heap;
        }
        for (size_t index = 0; index < lists.size(); ++index)
        {
            natives[index] = lists[index].native();
        }
        m_queue.submit(natives, wait, signal);
    }

    //==================================================================================================================
    void TracedQueue::present(Swapchain swapchain)
    {
        traceCall("present");
        m_queue.present(swapchain);
    }

    //==================================================================================================================
    void TracedQueue::mapTiles(std::span<const TileMapping> mappings)
    {
        traceCall("mapTiles");
        m_queue.mapTiles(mappings);
    }
} // namespace rojoRHI::bench
