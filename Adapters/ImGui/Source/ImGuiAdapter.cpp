//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiAdapter.cpp
/// @brief Implements the Dear ImGui renderer: initialization, textures and draw-data recording through the public interface.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/ImGuiAdapter.h>

#include "FrameRing.h"
#include "ImGuiRender.h"
#include "ImGuiRoot.h"
#include "ImGuiShaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace rojoRHI::imgui {
    using detail::fatal;
    using detail::g_state;
    using detail::kPlatformFormat;
    using detail::State;

    detail::State* detail::g_state = nullptr;

    //==================================================================================================================
    void detail::fatal(const std::string& message)
    {
        std::fprintf(stderr, "rojoRHI::imgui: %s\n", message.c_str());
        std::abort();
    }

    namespace {
        /// The GPU objects behind one ImGui texture this adapter created, held in `ImTextureData::BackendUserData`.
        struct TextureRecord
        {
            Texture texture;
            ViewIndex view;
        };

        constexpr uint64_t kVertexAlignment = 16;
        constexpr uint64_t kIndexAlignment = 16;
        constexpr uint64_t kStagingAlignment = 512;
        constexpr uint64_t kRowPitchAlignment = 256;
        constexpr uint32_t kBytesPerTexel = 4;

        //==============================================================================================================
        constexpr uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        //==============================================================================================================
        // An identifier the draw loop compares callbacks against; never called.
        void resetRenderState(const ImDrawList*, const ImDrawCmd*)
        {
        }

        //==============================================================================================================
        Result<Pipeline> createPipeline(Device device, ShaderLibrary library, Format format, const std::string& label)
        {
            const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = format,
                                                                  .blend = BlendState{ .enable = true,
                                                                                       .srcColor = BlendFactor::SrcAlpha,
                                                                                       .dstColor = BlendFactor::OneMinusSrcAlpha,
                                                                                       .colorOp = BlendOp::Add,
                                                                                       .srcAlpha = BlendFactor::One,
                                                                                       .dstAlpha = BlendFactor::OneMinusSrcAlpha,
                                                                                       .alphaOp = BlendOp::Add } } };
            return device.createPipeline(
                GraphicsPipelineDesc{ .vertex = { library, "imguiVertex" },
                                      .fragment = { library, "imguiFragment" },
                                      .primitive = PrimitiveClass::Triangle,
                                      .colors = colors,
                                      .label = label }
            );
        }

        //==============================================================================================================
        // Destroys every object `state` holds; each handle may still be null after a failed init.
        void destroyObjects(State& state)
        {
            state.ring.destroy();
            if (state.platformPipeline.bits != 0 && state.platformPipeline != state.hostPipeline)
            {
                state.device.destroy(state.platformPipeline);
            }
            if (state.hostPipeline.bits != 0)
            {
                state.device.destroy(state.hostPipeline);
            }
            if (state.sampler)
            {
                state.device.destroy(*state.sampler);
            }
            if (state.library.bits != 0)
            {
                state.device.destroy(state.library);
            }
            if (state.fence.bits != 0)
            {
                state.device.destroy(state.fence);
            }
        }

        //==============================================================================================================
        void destroyTexture(Device device, ImTextureData* tex)
        {
            auto* record = static_cast<TextureRecord*>(tex->BackendUserData);
            IM_ASSERT(record != nullptr && tex->TexID == textureId(record->view));
            device.destroy(record->view);
            device.destroy(record->texture);
            delete record;
            tex->BackendUserData = nullptr;
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }

        //==============================================================================================================
        void createTexture(State& state, ImTextureData* tex)
        {
            IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
            const uint32_t width = static_cast<uint32_t>(tex->Width);
            const uint32_t height = static_cast<uint32_t>(tex->Height);
            const std::string label = std::format("{}.texture", state.label);
            const std::array<TextureData, 1> initial{ TextureData{
                .bytes = std::as_bytes(std::span(static_cast<const unsigned char*>(tex->GetPixels()), size_t(tex->GetSizeInBytes()))),
                .bytesPerRow = uint64_t(width) * kBytesPerTexel } };
            Result<Texture> texture = state.device.createTexture(
                TextureDesc{ .format = Format::RGBA8Unorm, .extent = { width, height, 1 }, .usage = TextureUsage::Sampled, .label = label }, initial
            );
            if (!texture)
            {
                fatal(std::format("creating the {}x{} texture {} failed: {}", width, height, label, texture.error().message));
            }
            Result<ViewIndex> view = state.device.createView(*texture, TextureViewDesc{});
            if (!view)
            {
                fatal(std::format("creating the view of texture {} failed: {}", label, view.error().message));
            }
            tex->BackendUserData = new TextureRecord{ *texture, *view };
            tex->SetTexID(textureId(*view));
            tex->SetStatus(ImTextureStatus_OK);
        }

        //==============================================================================================================
        // Copies the bounding rectangle of the queued updates through the frame buffer, in one copy; the texture is in
        // ShaderRead before and after. Copies of the individual rectangles would write one subresource several times, which
        // needs a barrier between each write; the pixels between the rectangles are current in the texture data, so copying
        // them again is correct.
        void updateTexture(State& state, CommandList& list, ImTextureData* tex)
        {
            const auto* record = static_cast<const TextureRecord*>(tex->BackendUserData);
            IM_ASSERT(record != nullptr);
            const ImTextureRect rect = tex->UpdateRect;
            const std::array<TextureBarrier, 1> toCopy{ TextureBarrier{ .texture = record->texture,
                                                                        .srcStage = Stage::FragmentShader,
                                                                        .srcAccess = Access::ShaderRead,
                                                                        .dstStage = Stage::Copy,
                                                                        .dstAccess = Access::CopyWrite,
                                                                        .oldLayout = Layout::ShaderRead,
                                                                        .newLayout = Layout::CopyDst } };
            list.barrier({}, {}, toCopy);
            if (rect.w != 0 && rect.h != 0)
            {
                const uint64_t rowBytes = uint64_t(rect.w) * kBytesPerTexel;
                const uint64_t rowPitch = alignUp(rowBytes, kRowPitchAlignment);
                const detail::FrameRing::Allocation staging = state.ring.allocate(rowPitch * rect.h, kStagingAlignment);
                const auto* source = static_cast<const std::byte*>(tex->GetPixelsAt(rect.x, rect.y));
                const int pitch = tex->GetPitch();
                for (uint32_t row = 0; row < rect.h; ++row)
                {
                    std::memcpy(staging.cpu + row * rowPitch, source + size_t(row) * size_t(pitch), size_t(rowBytes));
                }
                list.copyBufferToTexture(
                    staging.buffer,
                    BufferTextureLayout{ .offset = staging.offset, .rowPitch = static_cast<uint32_t>(rowPitch) },
                    record->texture,
                    TextureRegion{ .offset = { rect.x, rect.y, 0 }, .extent = { rect.w, rect.h, 1 } }
                );
            }
            const std::array<TextureBarrier, 1> toRead{ TextureBarrier{ .texture = record->texture,
                                                                        .srcStage = Stage::Copy,
                                                                        .srcAccess = Access::CopyWrite,
                                                                        .dstStage = Stage::FragmentShader,
                                                                        .dstAccess = Access::ShaderRead,
                                                                        .oldLayout = Layout::CopyDst,
                                                                        .newLayout = Layout::ShaderRead } };
            list.barrier({}, {}, toRead);
            tex->SetStatus(ImTextureStatus_OK);
        }

        //==============================================================================================================
        void setRenderState(CommandList& list, Pipeline pipeline, float width, float height)
        {
            list.setPipeline(pipeline);
            list.setViewport(Viewport{ .width = width, .height = height });
            list.setPrimitiveTopology(PrimitiveTopology::TriangleList);
        }
    } // namespace

    //==================================================================================================================
    Result<void> init(const InitDesc& desc)
    {
        IM_ASSERT(ImGui::GetCurrentContext() != nullptr && "rojoRHI::imgui::init needs a current ImGui context");
        IM_ASSERT(g_state == nullptr && "rojoRHI::imgui::init was already called");
        ImGuiIO& io = ImGui::GetIO();
        IM_ASSERT(io.BackendRendererUserData == nullptr && "a renderer backend is already initialized");

        State* state = new State{ .device = desc.device,
                                  .label = std::string(desc.label),
                                  .colorFormat = desc.colorFormat,
                                  .surfaces = desc.surfaces,
                                  .ring = detail::FrameRing(desc.device, std::string(desc.label)) };
        const auto fail = [state](Error error) -> Result<void> {
            destroyObjects(*state);
            delete state;
            return std::unexpected(std::move(error));
        };

        Result<ShaderLibrary> library = state->device.loadShaderLibrary(embedded::kImGuiLibrary, std::format("{}.library", state->label));
        if (!library)
        {
            return fail(library.error());
        }
        state->library = *library;
        Result<Pipeline> host = createPipeline(state->device, state->library, state->colorFormat, std::format("{}.pipeline", state->label));
        if (!host)
        {
            return fail(host.error());
        }
        state->hostPipeline = *host;
        state->platformPipeline = state->hostPipeline;
        if (state->colorFormat != kPlatformFormat)
        {
            Result<Pipeline> platform = createPipeline(state->device, state->library, kPlatformFormat, std::format("{}.pipeline.platform", state->label));
            if (!platform)
            {
                return fail(platform.error());
            }
            state->platformPipeline = *platform;
        }
        Result<SamplerIndex> sampler = state->device.createSampler(
            SamplerDesc{ .filter = SamplerFilter::Linear,
                         .mipFilter = SamplerFilter::Linear,
                         .address = SamplerAddress::ClampToEdge,
                         .label = std::format("{}.sampler", state->label) }
        );
        if (!sampler)
        {
            return fail(sampler.error());
        }
        state->sampler = *sampler;
        Result<Fence> fence = state->device.createFence(0, std::format("{}.fence", state->label));
        if (!fence)
        {
            return fail(fence.error());
        }
        state->fence = *fence;

        g_state = state;
        io.BackendRendererName = "rojoRHI::imgui";
        io.BackendRendererUserData = state;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports;
        ImGui::GetPlatformIO().DrawCallback_ResetRenderState = resetRenderState;
        detail::installPlatformWindowCallbacks();
        return {};
    }

    //==================================================================================================================
    void shutdown()
    {
        IM_ASSERT(g_state != nullptr && "rojoRHI::imgui::shutdown before init");
        // Every platform window's swapchain and surface go first, while the platform backend still owns its windows.
        ImGui::DestroyPlatformWindows();
        detail::removePlatformWindowCallbacks();
        State& state = *g_state;
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        for (ImTextureData* tex : platformIo.Textures)
        {
            if (tex->BackendUserData != nullptr)
            {
                destroyTexture(state.device, tex);
            }
        }
        destroyObjects(state);
        if (platformIo.DrawCallback_ResetRenderState == resetRenderState)
        {
            platformIo.DrawCallback_ResetRenderState = nullptr;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendRendererUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);
        delete g_state;
        g_state = nullptr;
    }

    //==================================================================================================================
    void newFrame()
    {
        IM_ASSERT(g_state != nullptr && "rojoRHI::imgui::newFrame before init");
        g_state->ring.newFrame();
        detail::resizePlatformWindows();
    }

    //==================================================================================================================
    void prepare(CommandList& list, ImDrawData* drawData)
    {
        IM_ASSERT(g_state != nullptr && "rojoRHI::imgui::prepare before init");
        if (drawData == nullptr || drawData->Textures == nullptr)
        {
            return;
        }
        State& state = *g_state;
        for (ImTextureData* tex : *drawData->Textures)
        {
            switch (tex->Status)
            {
            case ImTextureStatus_WantCreate:
                IM_ASSERT(tex->Format == ImTextureFormat_RGBA32 && "the adapter creates RGBA32 textures only");
                createTexture(state, tex);
                break;
            case ImTextureStatus_WantUpdates:
                IM_ASSERT(tex->Format == ImTextureFormat_RGBA32 && "the adapter updates RGBA32 textures only");
                updateTexture(state, list, tex);
                break;
            case ImTextureStatus_WantDestroy:
                // A texture never created has nothing to free; one unused for at least one frame is no longer named by a
                // list of this frame.
                if (tex->BackendUserData == nullptr)
                {
                    tex->SetStatus(ImTextureStatus_Destroyed);
                }
                else if (tex->UnusedFrames > 0)
                {
                    destroyTexture(state.device, tex);
                }
                break;
            default: break;
            }
        }
    }

    //==================================================================================================================
    void render(CommandList& list, ImDrawData* drawData)
    {
        IM_ASSERT(g_state != nullptr && "rojoRHI::imgui::render before init");
        if (drawData == nullptr)
        {
            return;
        }
        const float width = std::round(drawData->DisplaySize.x * drawData->FramebufferScale.x);
        const float height = std::round(drawData->DisplaySize.y * drawData->FramebufferScale.y);
        if (width <= 0.0f || height <= 0.0f)
        {
            return;
        }
        detail::renderDrawData(
            list, drawData, g_state->hostPipeline, Extent2D{ .width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height) }
        );
    }

    //==================================================================================================================
    ImTextureID textureId(ViewIndex view)
    {
        return ImTextureID(static_cast<uint32_t>(view)) + 1;
    }

    //==================================================================================================================
    ViewIndex viewIndex(ImTextureID id)
    {
        return ViewIndex(static_cast<uint32_t>(id - 1));
    }

    //==================================================================================================================
    void detail::renderDrawData(CommandList& list, ImDrawData* drawData, Pipeline pipeline, Extent2D target)
    {
        IM_ASSERT(g_state != nullptr && "rojoRHI::imgui::render before init");
        if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f || drawData->TotalVtxCount == 0 || target.width == 0 ||
            target.height == 0)
        {
            return;
        }
        State& state = *g_state;
        const ImVec2 origin = drawData->DisplayPos;
        const ImVec2 size = drawData->DisplaySize;
        // The display fills the target. A platform window resized since newFrame draws stretched into its old extent until
        // the next newFrame resizes it, so pixels are counted in the target, not in the draw data's framebuffer size.
        const float framebufferWidth = float(target.width);
        const float framebufferHeight = float(target.height);
        const ImVec2 framebufferScale(framebufferWidth / size.x, framebufferHeight / size.y);
        setRenderState(list, pipeline, framebufferWidth, framebufferHeight);

        // ImGui pixels, y down from DisplayPos, to clip space, +y up: x' = 2 (x - left) / width - 1, y' = 1 - 2 (y - top) / height.
        const float scale[2] = { 2.0f / size.x, -2.0f / size.y };
        const float translate[2] = { -1.0f - origin.x * scale[0], 1.0f - origin.y * scale[1] };
        const IndexType indexType = sizeof(ImDrawIdx) == 2 ? IndexType::UInt16 : IndexType::UInt32;

        for (const ImDrawList* drawList : drawData->CmdLists)
        {
            if (drawList->VtxBuffer.Size == 0)
            {
                continue;
            }
            const uint64_t vertexBytes = uint64_t(drawList->VtxBuffer.Size) * sizeof(ImDrawVert);
            const uint64_t indexBytes = uint64_t(drawList->IdxBuffer.Size) * sizeof(ImDrawIdx);
            const detail::FrameRing::Allocation vertices = state.ring.allocate(vertexBytes, kVertexAlignment);
            std::memcpy(vertices.cpu, drawList->VtxBuffer.Data, size_t(vertexBytes));
            const detail::FrameRing::Allocation indices = state.ring.allocate(indexBytes, kIndexAlignment);
            if (indexBytes > 0)
            {
                std::memcpy(indices.cpu, drawList->IdxBuffer.Data, size_t(indexBytes));
            }
            const uint32_t firstIndexBase = static_cast<uint32_t>(indices.offset / sizeof(ImDrawIdx));

            for (const ImDrawCmd& cmd : drawList->CmdBuffer)
            {
                if (cmd.UserCallback != nullptr)
                {
                    if (cmd.UserCallback == resetRenderState)
                    {
                        setRenderState(list, pipeline, framebufferWidth, framebufferHeight);
                    }
                    else
                    {
                        cmd.UserCallback(drawList, &cmd);
                    }
                    continue;
                }
                // Whole pixels covering the clip rectangle, clamped to the framebuffer.
                const float x0 = std::clamp(std::floor((cmd.ClipRect.x - origin.x) * framebufferScale.x), 0.0f, framebufferWidth);
                const float y0 = std::clamp(std::floor((cmd.ClipRect.y - origin.y) * framebufferScale.y), 0.0f, framebufferHeight);
                const float x1 = std::clamp(std::ceil((cmd.ClipRect.z - origin.x) * framebufferScale.x), 0.0f, framebufferWidth);
                const float y1 = std::clamp(std::ceil((cmd.ClipRect.w - origin.y) * framebufferScale.y), 0.0f, framebufferHeight);
                const Rect2D scissor{ .x = static_cast<uint32_t>(x0),
                                      .y = static_cast<uint32_t>(y0),
                                      .width = x1 > x0 ? static_cast<uint32_t>(x1) - static_cast<uint32_t>(x0) : 0,
                                      .height = y1 > y0 ? static_cast<uint32_t>(y1) - static_cast<uint32_t>(y0) : 0 };
                if (scissor.width == 0 || scissor.height == 0 || cmd.ElemCount == 0)
                {
                    continue;
                }
                list.setScissor(scissor);
                const ImTextureID id = cmd.GetTexID();
                IM_ASSERT(id != ImTextureID_Invalid && "a draw command names no texture");
                const detail::Root root{ .vertices = vertices.address + uint64_t(cmd.VtxOffset) * sizeof(ImDrawVert),
                                         .scale = { scale[0], scale[1] },
                                         .translate = { translate[0], translate[1] },
                                         .texture = static_cast<uint32_t>(viewIndex(id)),
                                         .sampler = static_cast<uint32_t>(*state.sampler) };
                list.pushRoot(root);
                list.drawIndexed(indices.buffer, indexType, cmd.ElemCount, 1, firstIndexBase + cmd.IdxOffset, 0, 0);
            }
        }
    }
} // namespace rojoRHI::imgui
