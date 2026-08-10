//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ImGui.cpp
/// @brief Bridges Dear ImGui rendering and texture residency to the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------
#include "RHI/Metal4/Metal4ImGui.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Metal4CommandList.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Resources.h"

// Keep the Objective-C surface in imgui_impl_metal4.mm; this TU uses its metal-cpp declarations.
#include <imgui_impl_metal4.h>

#include <cstdint>
#include <utility>

namespace lmx::rhi::metal4 {
namespace {

// The backend stores its state in the current ImGui context and exposes no instance handle.
Metal4Device* g_device = nullptr;

// NewFrame keys its pipeline cache from attachment formats and sample count. A 1x1 carrier
// supplies that metadata without acquiring the drawable before the UI decides whether to render.
NS::SharedPtr<MTL::Texture> g_formatCarrier;
NS::SharedPtr<MTL4::RenderPassDescriptor> g_passDescriptor;

// Track the slot, not just pairing: reusing ImGui's previous per-frame buffers can race the GPU.
constexpr uint32_t kNoFrameStarted = UINT32_MAX;
uint32_t g_pendingFrameSlot = kNoFrameStarted;

} // namespace

//======================================================================================================================
bool imguiInit(Device& device, Format colorFormat) {
    // The backend is non-ARC and issues autoreleased Objective-C calls.
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(g_device == nullptr, "imguiInit: already initialised -- call imguiShutdown first");
    // Reject unsupported formats before Metal's texture-descriptor validator aborts.
    LMX_ASSERT(colorFormat == Format::BGRA8Unorm || colorFormat == Format::RGBA8Unorm,
               "imguiInit: colorFormat must be a color-renderable format (BGRA8Unorm or "
               "RGBA8Unorm)");

    auto& metalDevice = static_cast<Metal4Device&>(device);

    // The metadata-only carrier needs neither an RHI wrapper nor residency registration.
    auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    textureDesc->setTextureType(MTL::TextureType2D);
    textureDesc->setPixelFormat(toMTL(colorFormat));
    textureDesc->setWidth(1);
    textureDesc->setHeight(1);
    textureDesc->setMipmapLevelCount(1);
    // ImGui reads sample count from the attachment texture, not the pass descriptor default.
    textureDesc->setSampleCount(1);
    // Match the attachment role used to compile the UI pipeline.
    textureDesc->setUsage(MTL::TextureUsageRenderTarget);
    textureDesc->setStorageMode(MTL::StorageModePrivate);

    NS::SharedPtr<MTL::Texture> carrier =
        NS::TransferPtr(metalDevice.handle()->newTexture(textureDesc.get()));
    if (!carrier) {
        LMX_LOG_ERROR("ImGui init: failed to create the 1x1 render-pass format carrier texture");
        return false;
    }
    carrier->setLabel(makeString("lmx.imgui.formatCarrier").get());

    auto passDesc = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
    passDesc->colorAttachments()->object(0)->setTexture(carrier.get());

    // ImGui's buffer ring must rotate with the device's allocator and argument-table ring.
    if (!ImGui_ImplMetal4_Init(metalDevice.handle(), metalDevice.queue(),
                               static_cast<int>(kFramesInFlight))) {
        LMX_LOG_ERROR("ImGui init: the Metal 4 renderer backend refused to initialise");
        return false;
    }

    g_formatCarrier = std::move(carrier);
    g_passDescriptor = std::move(passDesc);
    g_device = &metalDevice;
    LMX_LOG_INFO("ImGui renderer: imgui_impl_metal4, {} frames in flight", kFramesInFlight);
    return true;
}

//======================================================================================================================
void imguiShutdown() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Shutdown is intentionally safe after partial initialization.
    if (g_device == nullptr) {
        return;
    }

    // The backend frees cached buffers immediately; drain frames that may still reference them.
    g_device->waitIdle();

    ImGui_ImplMetal4_Shutdown();
    g_passDescriptor.reset();
    g_formatCarrier.reset();
    g_pendingFrameSlot = kNoFrameStarted;
    g_device = nullptr;
}

//======================================================================================================================
void imguiNewFrame() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(g_device != nullptr, "imguiNewFrame: call imguiInit first");
    // Before beginFrame(), this returns the slot about to open rather than the one just submitted.
    const uint32_t slot = g_device->frameInFlightIndex();
    ImGui_ImplMetal4_NewFrame(g_passDescriptor.get(), static_cast<int>(slot));
    g_pendingFrameSlot = slot;
}

//======================================================================================================================
void imguiRender(CommandList& commands) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(g_device != nullptr, "imguiRender: call imguiInit first");
    auto& commandList = static_cast<Metal4CommandList&>(commands);

    ImDrawData* drawData = ImGui::GetDrawData();
    LMX_ASSERT(drawData != nullptr,
               "imguiRender: no draw data -- call ImGui::Render() before this, in the same frame");

    // The open frame must own the same buffer slot prepared by imguiNewFrame().
    LMX_ASSERT(g_pendingFrameSlot != kNoFrameStarted,
               "imguiRender: no ImGui frame is open -- call imguiNewFrame() (then ImGui::NewFrame, "
               "build the UI, ImGui::Render) once per frame before this");
    LMX_ASSERT(g_pendingFrameSlot == g_device->frameInFlightIndex(),
               "imguiRender: this frame's imguiNewFrame() ran against a different frame in flight "
               "-- call imguiNewFrame() and imguiRender() exactly once each per Device frame");
    g_pendingFrameSlot = kNoFrameStarted;

    ImGui_ImplMetal4_RenderDrawData(drawData, commandList.commandBuffer(),
                                    commandList.currentEncoder());
}

//======================================================================================================================
ImTextureID imguiTextureID(Texture& texture) {
    LMX_ASSERT(g_device != nullptr, "imguiTextureID: call imguiInit first");

    // ImGui expects the Objective-C texture object pointer and derives ResourceID during drawing.
    MTL::Texture* handle = static_cast<Metal4Texture&>(texture).handle();
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(handle));
}

//======================================================================================================================
void imguiForgetTexture(Texture& texture) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Texture teardown remains safe when the UI was never initialized.
    if (g_device == nullptr) {
        return;
    }
    // Use the backend extension so residency bookkeeping stays encapsulated.
    MTL::Texture* handle = static_cast<Metal4Texture&>(texture).handle();
    LMX_ASSERT(ImGui_ImplMetal4_RemoveTexture(handle),
               "imguiForgetTexture: Metal backend is not initialized");
}

} // namespace lmx::rhi::metal4
