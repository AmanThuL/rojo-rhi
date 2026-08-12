//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ImGui.h
/// @brief Declares the optional Dear ImGui bridge for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/CommandList.h"
#include "RHI/Device.h"
#include "RHI/Format.h"
#include "RHI/Texture.h"

#include <imgui.h>

namespace lmx::rhi::metal4 {

/// Wires Dear ImGui's renderer backend to `device`'s Metal device and command queue, sized for this
/// backend's three frames in flight. Call once, after ImGui::CreateContext() and before anything
/// else here. Returns false, after logging why, if the backend could not be created; nothing is
/// left initialised on that path.
///
/// `colorFormat` is the pixel format of the render pass imguiRender() will be called inside -- the
/// swapchain's in the ordinary case, which is why it defaults to what SwapchainDesc defaults to.
/// It is wanted at init rather than at render time because Dear ImGui keys its UI pipeline on a
/// framebuffer description it is handed before the frame's real target exists; see imguiNewFrame().
///
/// That pass must also be single-sampled and carry no depth or stencil attachment, which is how
/// this glue describes it. A mismatch is not silent but neither is it catchable here: it surfaces
/// as a Metal validation failure about the render pipeline being incompatible with the render pass.
///
/// Returns bool rather than Result<T> despite the creation-shaped name, following beginCapture():
/// this is tooling the App can run without, not an RHI object whose failure a caller must model.
bool imguiInit(Device& device, Format colorFormat = Format::BGRA8Unorm);

/// Releases everything imguiInit() created, including the font atlas texture, and unregisters the
/// renderer from the ImGui context. Call before ImGui::DestroyContext().
///
/// Drains the device itself before freeing anything -- the ImGui backend frees its textures and
/// buffers without waiting, so a frame still in flight would be reading freed allocations. The
/// caller does not have to waitIdle first. No-op when imguiInit() was never called or has already
/// been undone, so an early-exit teardown path needs no bookkeeping of its own.
void imguiShutdown();

/// Opens ImGui's renderer frame. Call once per frame, immediately before ImGui::NewFrame().
///
/// Callable before Device::beginFrame(), and required to be: Dear ImGui wants the renderer's
/// NewFrame ahead of ImGui::NewFrame(), which is ahead of the UI-building code that decides what
/// the frame draws at all. Two consequences follow, both absorbed here rather than pushed onto the
/// caller. The frame-in-flight slot handed to the backend is the slot of the frame *about to open*
/// rather than the one that just ended (Metal4Device::frameInFlightIndex() gets that phase right on
/// both sides of beginFrame). And the framebuffer description handed to the backend is a
/// format-only stand-in built at init, because the frame's real render target does not exist yet.
void imguiNewFrame();

/// Encodes ImGui::GetDrawData() into the render pass currently open on `commands`. Call after
/// ImGui::Render(), between beginRenderPass and endRenderPass; asserts when no pass is open, when
/// ImGui::Render() has not run, and when this frame's imguiNewFrame() is missing or belonged to a
/// different frame in flight. That last check is not redundant bookkeeping: the ImGui backend holds
/// its frame slot in a member, so an unpaired render would silently reuse the previous frame's slot
/// and write buffers the GPU may still be reading -- the one misuse here with no other symptom.
///
/// This hands the encoder to ImGui, and ImGui does not put it back: the argument table, render
/// pipeline state, depth-stencil state, viewport, scissor rect and cull mode all belong to ImGui
/// afterwards. Anything drawn later in the same pass must rebind its own. Making this the last call
/// in the pass -- what the App does -- sidesteps the question.
void imguiRender(CommandList& commands);

/// Returns the ImTextureID naming an RHI texture for ImGui::Image and related calls.
///
/// The texture must have been created with TextureDesc::sampled, and must outlive every frame whose
/// draw data still references it -- ImGui holds the identifier, never a reference. Residency for
/// the draw itself is ImGui's problem and it handles it: the texture joins the backend's own
/// residency set when the draw command that names it is encoded -- and never leaves it on its own,
/// which is what imguiForgetTexture() below exists for.
ImTextureID imguiTextureID(Texture& texture);

/// Removes `texture` from the ImGui backend's residency set before releasing the texture.
/// any texture that has been displayed through imguiTextureID().
///
/// Upstream only adds user textures. The pinned backend patch exposes the matching removal as a
/// supported function, keeping its residency-set ownership inside the backend instead of exposing
/// backend storage to this wrapper. Without the removal, a resized editor viewport leaves every
/// old color target alive and resident for the process lifetime.
///
/// The caller guarantees the GPU is done with the texture (Device::waitIdle) -- it is about to be
/// freed anyway. No-op when imguiInit() was never called, so a UI-less path (--screenshot) needs no
/// guard of its own.
void imguiForgetTexture(Texture& texture);

} // namespace lmx::rhi::metal4
