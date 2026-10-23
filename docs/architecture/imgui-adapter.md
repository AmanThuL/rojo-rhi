# The ImGui Adapter

**Status**: Accepted

`RojoRHIImGui` renders Dear ImGui, platform windows included, through the public interface alone.
It has no Metal, D3D12 or Null code: it builds and runs on whichever backend the library was
configured for. D3D12 is implemented, verified on WARP only, not on hardware, so the adapter's D3D12
runs are WARP runs and are not T1 evidence. The v1 adapter under `Legacy/Backends/Metal4/ImGui/` is
a different, Metal-only target that stays until `Legacy/` is deleted ([overview](overview.md)).

## Purpose and boundary

The adapter lives in `Adapters/ImGui/` with three parts: `Include/rojoRHI/ImGuiAdapter.h`,
`Source/` and `Shaders/ImGui.slang`. It includes `<rojoRHI/RHI.h>` and `<imgui.h>`, and nothing from
`Source/` or `Backends/`. It holds no `ROJORHI_VU`: a misuse of the interface it makes is caught by
the core's checks, and a misuse of the ImGui protocol is an `IM_ASSERT`
(`Tools/check_vu_coverage.py` rejects a `ROJORHI_VU` under `Adapters/`).

The adapter header includes `<imgui.h>`, so it cannot live under `Include/`, which stays free of
any third-party type (ADR 0007, ADR 0012). Its include root, `Adapters/ImGui/Include`, is a separate
directory that only the adapter target exports, and `Tools/check_rhi_headers.py` still checks
`Include/` alone. The public spelling is `<rojoRHI/ImGuiAdapter.h>`, the namespace `rojoRHI::imgui`.

## The target

A host names its Dear ImGui target in `rojorhi_imgui_target` before it includes
`xmake/targets.lua`. Where the variable is set, `targets.lua` declares:

- `RojoRHIImGui`, a static target that depends on `RojoRHI` and on the host's ImGui target. It is
  `set_default(false)` while `Legacy/` exists, so a host's plain build keeps building the v1 adapter
  it links. It has its own `targetdir` (`rojorhi-imgui`), so its `Shaders/` output stays apart from
  the test binary's, and it embeds its shader library into `ImGuiShaders.h` through the
  `rojorhi.slang` rule (`slang.embed`).
- the `[imgui]` cases in `RojoRHITests` (see Tests below).

While `Legacy/` exists, `RojoRHILegacyMetal4ImGui` is declared only when the ImGui target is not
`RojoRHIDearImGui`: the v1 adapter needs the host's patched Metal backend header, which an
unpatched checkout lacks.

## The host loop

Per frame, in this order:

1. `imgui::newFrame()`, with no device frame open. It frees buffers retired by growth and resizes
   platform-window swapchains.
2. The platform backend's `NewFrame`, then `ImGui::NewFrame()`, the host's UI, `ImGui::Render()`.
3. `Device::beginFrame`.
4. `imgui::prepare(list, drawData)`, outside a render pass: it creates, updates and destroys
textures.
5. The host's render pass, with `imgui::render(list, drawData)` inside it. The pass area must cover
   `DisplaySize` times `FramebufferScale`, or a scissor falls outside it.
6. `imgui::renderPlatformWindows(list)`, outside a render pass, after the host's pass ends.
7. End and submit the list; present the host's own swapchain.
8. `imgui::presentPlatformWindows(queue)`, after that submission.
9. `Device::endFrame`.
10. `ImGui::UpdatePlatformWindows()`, outside any frame. ImGui creates and closes platform windows
    here, which is why this step follows `endFrame`.

`init` runs after `ImGui::CreateContext` and the platform backend's init, and returns a
`Result<void>`. `shutdown` runs outside a frame while the device is alive. It calls
`ImGui::DestroyPlatformWindows()` first, which also runs the platform backend's `DestroyWindow`
for the main viewport (SDL3 frees its data there and destroys no window), so the main viewport has
no `PlatformHandle` afterwards and a second `init` needs the platform backend initialised again.
Either order with the platform backend's own shutdown works while the device is alive. At most one
`newFrame` per device frame; a window created in frame N is first drawn in frame N + 1.

## The frame buffer ring

`FrameRing` owns three slots, each one growable `Memory::Upload` buffer created with
`BufferUsage::Index`. `Index` is valid on `Upload`, and every `Upload` buffer is a copy source and
readable by address, so one buffer serves as vertex storage, index buffer and texture staging.
Each frame bump-allocates from the current slot: vertices and indices at 16-byte alignment, texture
rows at a 512-byte offset with a 256-byte row pitch (the copy alignment of chapter 07).

**Three slots.** Three is the largest `DeviceDesc::framesInFlight`, so the adapter needs no
frames-in-flight input. `newFrame` only arms an advance: the first allocation after it moves to the
next slot and rewinds it. The ring therefore advances once per frame that wrote, and a frame that
wrote is a device frame. Four writing frames D1 < D2 < D3 < D4 reuse D1's slot only at
D4 >= D1 + 3, after `beginFrame` of D4 waited for D1's slot to retire. A host that calls `newFrame`
on frames with no device frame, a minimized window for instance, consumes no slot.

**Growth.** A request that does not fit moves the slot's buffer to a retired list and creates a
new one with capacity `max(2 x old, alignUp(request, 64 KiB), 256 KiB)`, at offset 0. Data written
earlier in the frame stays in the old buffer, which commands already recorded still read. Retired
buffers are destroyed at the next `newFrame`, never mid-frame: a buffer an unsubmitted list
references must not be destroyed, and by the next `newFrame` every list of the previous frame was
submitted. A growth failure is fatal (see Failures).

**Draw arithmetic.** Per command the root holds the address
`Device::address(buffer) + vertexOffset + cmd.VtxOffset * sizeof(ImDrawVert)`, and `drawIndexed`
passes `baseVertex = 0` and `firstIndex = indexOffset / sizeof(ImDrawIdx) + cmd.IdxOffset`. The
vertex entry reads `rojoRHI::vertexIndex`, which excludes the base vertex (ADR 0024); the adapter
folds `VtxOffset` into the address instead of the base, so a mesh past 64K vertices with 16-bit
indices reads the right vertices on every backend. The scissor is rounded to whole pixels (floor
of the start, ceil of the end), clamped to the target, and a command whose clamped scissor is empty
is skipped. A draw list with no vertex records nothing.

## Textures

ImGui owns each `ImTextureData`; the adapter creates the GPU side in `prepare`, from
`drawData->Textures`:

- `WantCreate`: `createTexture(Tex2D, RGBA8Unorm, Sampled)` with the pixels as initial data (the
  texture starts in `ShaderRead`), a view, `SetTexID(textureId(view))`, status OK.
- `WantUpdates`: the rows of `UpdateRect`, the bounding box of `Updates`, are copied into ring
  staging, a barrier moves the texture `ShaderRead` to `CopyDst`, one `copyBufferToTexture` runs
  and a second barrier restores `ShaderRead`. A copy per rectangle would write the subresource
  several times, each write needing a barrier before the next; the pixels between the rectangles
  are current in the texture data, so the one copy is correct, as in Dear ImGui's Vulkan backend.
- `WantDestroy` with `UnusedFrames > 0`: destroy the view and the texture, `SetTexID` to
  `ImTextureID_Invalid`, status Destroyed.
- `shutdown` sets the status Destroyed on the textures it created. ImGui's `SetStatus` turns that
  into `WantCreate` for a texture that keeps its pixels and is not scheduled for destruction, so a
  later `init` creates it again.
- A format other than `ImTextureFormat_RGBA32` is an `IM_ASSERT`.

A texture the host owns is a `ViewIndex` that the host keeps in `Layout::ShaderRead` while draw data
names it. Destroying its view is deferred by the device, so the host only avoids destroying a view
that a list not yet submitted references.

## `ImTextureID`

`textureId(view)` returns the view index plus one, and `viewIndex(id)` subtracts one.
`ImTextureID_Invalid` is 0 and view index 0 is a valid heap slot, so the offset keeps view 0
distinguishable from "no texture". Both work at any time, before `init` included.

## Platform windows

`init` installs `Renderer_CreateWindow`, `Renderer_DestroyWindow` and `Renderer_SetWindowSize` in
`ImGuiPlatformIO`. The host supplies `SurfaceCallbacks`: `create` returns the native surface (a
`CA::MetalLayer*` on Metal 4, an `HWND` on D3D12) and `destroy` releases it.

- **Creation.** `Renderer_CreateWindow` calls `create`, then `createSwapchain` with
  `BGRA8Unorm`, `ColorSpace::SRGB`, three images and `Fifo`. A failure is fatal. The adapter cannot
  read a swapchain's extent, so it records it as unknown until `newFrame`.
- **Resize.** The OS resizes windows behind ImGui's back, so `Renderer_SetWindowSize` does nothing.
  `newFrame` compares each swapchain's recorded extent with the window's size times its framebuffer
  scale, rounded, both read from the platform's `Platform_GetWindowSize` and
  `Platform_GetWindowFramebufferScale` (the viewport's `Size` and `FramebufferScale` where the
  platform installs neither), and calls `Swapchain::resize`, which waits for the graphics queue;
  it is called with no frame open and no image held. The viewport's own values lag by a frame:
  `ImGui::NewFrame` refreshes them after `newFrame`, while the OS has already resized the surface,
  on which a swapchain of the old extent may acquire nothing (Metal 4 waits up to a second for a
  drawable of its size). During a live resize the window draws every frame against its swapchain
  extent, stretched when ImGui's size moved after `newFrame`; the extent catches up at the next
  `newFrame`.
- **Minimized and zero size.** `newFrame` does not resize a window that is flagged minimized,
  whose platform reports it minimized before `ImGui::NewFrame` refreshes the flag, or whose extent
  is zero.
  Drawing skips a viewport flagged `ImGuiViewportFlags_IsMinimized` or with a zero extent: it
  acquires nothing and presents nothing.
- **Destruction.** Closing a window destroys its swapchain, submits an empty submission to the
  graphics queue that signals the adapter's fence, waits for it (fatal after 5 s) and then calls
  `SurfaceCallbacks::destroy`. The reference requires the native surface to outlive
  `Device::destroy(swapchain)` and every frame in flight at that call, while SDL destroys the window
  right after the renderer callback returns. The wait is the price, one GPU wait per closed window.
  `destroy` runs once per closed window and never for the main viewport.
- **Format and colour space.** A platform pass uses `BGRA8Unorm`, the format the SRGB colour space
  lists. When `InitDesc::colorFormat` differs from it, `init` creates a second pipeline for platform
  windows. The pass clears to opaque black, or loads nothing (`DontCare`) under
  `ImGuiViewportFlags_NoRendererClear`, because an acquired image has undefined contents.

Blending is `SrcAlpha`/`OneMinusSrcAlpha` for colour and `One`/`OneMinusSrcAlpha` for alpha, with no
depth and no culling. `ImDrawCallback_ResetRenderState` re-sets the pipeline, viewport and topology;
any other draw callback is called with the list and the command.

## Failures and checks

`init` returns its creation failures (`loadShaderLibrary`, `createPipeline`, `createSampler`,
`createFence`) as `Result<void>`, after destroying what it made. A creation failure inside a frame
entry (ring growth, a texture create, a platform swapchain) is fatal: a message on stderr, then
`std::abort()`, as the library's own `uploadFrameData` growth failure is. Nothing is swallowed.
Calling an entry before `init`, `init` twice, a second renderer backend and an unsupported texture
format are `IM_ASSERT`s. The adapter carries no valid-usage identifier, because every check that
guards a library entry lives in the core.

## The shader and the root block

`ImGui.slang` has two entries, `imguiVertex` and `imguiFragment`, and declares no binding: the
module owns them (ADR 0018). The vertex entry reads one `ImDrawVert` with `rojoRHI::load` at
`vertexIndex`, and the fragment multiplies the vertex colour by one sample of the command's view
through the command's sampler. The root is flat, 32 bytes: the vertex address at 0, scale at 8,
translate at 16, the view index at 24 and the sampler index at 28.

C++ and Slang each declare the layout, with no reflection probe between them. `static_assert`s pin
`sizeof(Root)` and each member's offset, and `sizeof(ImDrawVert) == 20` with its three member
offsets. The `[imgui]` pixel case compares every pixel of a fixed draw list at tolerance 0, so a
drift on the shader side shows as a mismatch.

## Tests

The cases are tagged `[imgui]` and live in `Tests/ImGui/`: the ring (slot reuse, a frame that writes
nothing, growth), `init` and `shutdown`, `textureId`, texture create/update/destroy, a fake platform
backend driving two viewports (create, close, resize, an OS resize ImGui has not seen, minimize,
consecutive resizes) and one pixel case. They build into `RojoRHITests` and run as
`xmake test RojoRHITests/imgui`.

| Where | What runs |
|---|---|
| Null, macOS and Windows | every case except the pixel case, which skips without a GPU |
| Metal 4, locally | every case, under `MTL_DEBUG_LAYER=1`; a validation message fails the run |
| d3d12 CI job | every case on WARP, Debug and Release; WARP is not hardware |

The `metal4` CI job compiles and inventories the entry without running it.

## Standalone setup

`xmake setup` fetches Dear ImGui at the pin `83f668625ad45364de71d385aeb6a5dd04bee02e` into
`ThirdParty/imgui` on every host with git, then checks that `HEAD` equals the pin and that ten core
files are clean: `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui.h`,
`imgui_internal.h`, `imconfig.h`, `imstb_rectpack.h`, `imstb_textedit.h` and `imstb_truetype.h`.
`backends/` is not checked: the adapter compiles none of it. The standalone root sets
`rojorhi_imgui_target = "RojoRHIDearImGui"`, declared in `xmake/imgui.lua`: a static, non-default
target of the four core `.cpp` files, with warnings off and the include directory public as a system
include, so `imgui.h` raises none under `/WX`. It exists for the tests; a host never includes
that file.
