# The D3D12 Backend: Presentation, Sparse Resources, Capture and FSR

**Status**: Accepted

Implemented, verified on WARP only, not on hardware: [CI run 39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757) ran these entries on the hosted
Windows runner's WARP adapter under the debug layer and GPU-based validation, and no run has used a
hardware adapter. The backend is not a capability until a hardware run says so
([ADR 0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)). Under GPU-based validation WARP never completes
an FSR dispatch; with the debug layer alone it does.

This page describes swapchains and presentation, reserved (sparse) resources, programmatic capture
and FSR: upscaler creation and the recording of an upscale.
[The D3D12 backend](d3d12-backend.md) describes the rest of the backend, and chapters 11 to 14 of
the API reference stay the authority on semantics and mapping. Each entry calls the core's check
first and then translates; the backend holds no `ROJORHI_VU`. [The Null backend's
edges](null-backend-edges.md) describe the shared core these entries call.

## Presentation

A swapchain is an `IDXGISwapChain3` on a window (`desc.nativeSurface`, an `HWND`).
`createSwapchain` calls `core::prepareSwapchain` first, so the surface check of
`VU-rojo-present-0002` (`IsWindow`) runs before the handle is read, and claims the window for the
core (`VU-rojo-present-0006`) before any native call. `createNative` then:

1. Takes the present flags of the mode (`d3d12::presentFlags`). Every mode creates the swapchain
   with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`; `Fifo` presents with interval 1,
   `Mailbox` with interval 0, and `Immediate` with interval 0 and `DXGI_PRESENT_ALLOW_TEARING` on a
   swapchain that also has `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`. The tearing feature is queried
   only for `Immediate`, through `IDXGIFactory5::CheckFeatureSupport` of the device's factory;
   without `DXGI_FEATURE_PRESENT_ALLOW_TEARING` creation returns `Unsupported` naming it.
2. Calls `CreateSwapChainForHwnd` on the graphics queue with width and height 0 (the window's
   client extent), the descriptor's format, `FLIP_DISCARD`, `BufferCount = imageCount`,
   `DXGI_SCALING_STRETCH` and `DXGI_ALPHA_MODE_UNSPECIFIED`. The extent `GetDesc1` reports becomes
   the core's extent.
3. Sets the color space: `SRGB` is `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`, `ExtendedLinear` is
   `..._G10_NONE_P709` and `HDR10` is `..._G2084_NONE_P2020`. `CheckColorSpaceSupport` runs first;
   a space the output does not report with `DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT`
   returns `Unsupported` naming the `ColorSpace` and its DXGI space, and `SetColorSpace1` follows.
4. Calls `SetMaximumFrameLatency(maxLatency)` and `MakeWindowAssociation(hwnd,
   DXGI_MWA_NO_ALT_ENTER)`, then fetches the waitable object.
5. Makes a CPU-only render-target heap `<label>.rtv` with one view per back buffer, and fetches
   each back buffer with `GetBuffer`, names it `<label>.image<i>` and writes its view. Each view
   write counts one `descriptorWrites`.

A failure at any step removes the core's claim, frees what was made and returns the error:
`Unsupported` for a mode or color space, else the native call's error.

**Acquire.** `Swapchain::acquire` runs `core::checkAcquireImage`, then waits on the waitable
object for at most one second, `kAcquireDeadlineMs`, counted as one `nativeHostCalls`. The object
is signalled while fewer than `maxLatency` presents are queued. A timeout returns a null
`Texture` and changes no state; any other result logs and aborts. On success it reads
`GetCurrentBackBufferIndex` and gives the core the payload of that back buffer.

**Borrowed back buffers.** The texture `acquire` returns borrows the back buffer and its
render-target view: its payload belongs to the swapchain, and its record's `swapchainImage` flag
makes the texture's retire hook release nothing. The swapchain owns the buffers, the heap and the
payloads and drops them itself. Each payload holds the swapchain's format, `RenderTarget` usage and
current extent, set again by `resize`, which render passes and clears of the image read.

**Present.** `Queue::present` runs `core::presentImage`, which releases the image's texture
deferred, then `IDXGISwapChain::Present(interval, flags)`, one `nativeSubmitCalls`. `S_OK` and
`DXGI_STATUS_OCCLUDED` (a window the display does not show) are successful presents. A device
removal reports the removal reason and aborts; any other result logs the `HRESULT` and aborts. The
last result is kept in `SwapchainNative::presented` so the `[d3d12]` cases can read whether
occlusion happened.

**Resize.** After `core::resizeSwapchain`, `Swapchain::resize` waits, when a frame has been
numbered, for the frame fence to reach `frameNumber()`: the graphics queue signals it after every
list of the ended frame, so no work still references the back buffers. The wait counts its host
calls. Before the first frame there is nothing to wait for. It then releases every back-buffer
reference (`ResizeBuffers` fails while one is held), calls `ResizeBuffers` with creation's count,
format and flags, and fetches the buffers again, naming them again. A refused `ResizeBuffers`
logs its `HRESULT` and aborts.

**Retirement.** `Device::destroy(Swapchain)` runs `core::destroySwapchain` and files the swapchain
in `Presentation::retired` with `frameNumber()` (the open frame, or the last ended frame between
frames). Presents and the lists that use its images come only inside a frame, so waiting for that
frame completes them; the frame-slot hook frees, through `freePassedSwapchains`, every entry whose
frame the core has waited for. Filing by frame, not by slot, keeps a swapchain destroyed while a
`beginFrame` opens the next frame from being freed by that call's hook. A swapchain is no pool
record, so the core's retire loop cannot see it: the function counts one `deferredRetired` and one
`nativeCreateCalls` for each swapchain it frees. `destroyDevice` frees the live and retired
swapchains after the queues are idle and counts nothing.

**One window, one swapchain.** DXGI binds a window to one swapchain. `createSwapchain` on the
window of a destroyed swapchain that is still retired takes that swapchain out of its list, has the
graphics queue signal the `rojorhi.presentation` fence (made on first use), waits for it outside
the lock and frees the swapchain. The fence is its own: a frame fence would deadlock when the new
swapchain is created inside a frame that has not ended or before the first frame. This works inside
an open frame.

**Hidden-window test surfaces.** `test::surface(index)` (`Tests/Support/D3D12/D3D12Backend.cpp`)
makes up to eight overlapped windows of 256 x 256 pixels from one registered class, on first use,
alive for the process and never shown. A flip-model swapchain presents on them on the hosted
runner. A case that wants occlusion minimises a window and logs whether
`DXGI_STATUS_OCCLUDED` came back; it accepts every successful result.

## Sparse resources

`BufferUsage::Sparse` and `TextureUsage::Sparse` create reserved resources with
`CreateReservedResource2` in `D3D12_BARRIER_LAYOUT_UNDEFINED` (`D3D12Sparse.cpp`). A reserved
buffer is `Memory::Gpu`, its width rounded up to whole 64 KiB tiles so the last tile maps like any
other; its raw views cover the requested size, and it is a buffer like any other. A reserved
texture uses `D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE` and the castable formats of an ordinary
texture. Creation returns `Unsupported`, naming the cause, for a one-dimensional texture, for a 3D
texture on a device below `D3D12_TILED_RESOURCES_TIER_3`, and for a format the device does not
report sparse.

**Capabilities.** `placementSparse` is true at `TiledResourcesTier` 2 or higher: from tier 2 an
unmapped tile reads as zero and discards writes, and no mip of at least one tile is packed into the
tail ([sparse resources](../spec/13-sparse.md), tiled resources tiers). A format is `sparse` when
the core's ceiling allows it, `D3D12_FORMAT_SUPPORT2_TILED` is set and the format has no stencil
plane: D3D12 tiles each plane as its own subresources, which a `TileMapping` cannot name. 3D
textures are limited to tier 3 at creation, as above.

**Tile shapes.** `readTileShape` calls `GetResourceTiling` once at creation and keeps the result in
the texture record: the extents of `D3D12_TILE_SHAPE`, 65,536 bytes per tile, the first mip tail
level `NumStandardMips` and the tail's bytes, `NumTilesForPackedMips` tiles. A texture with no
packed mip (`NumPackedMips` is 0) has `mipTailFirst` equal to its mip count and no tail bytes. The
WARP sparse test texture reports no packed mips. `Device::tileShape` for a texture reads the record
and for a buffer is the core's answer; neither calls the runtime again or counts a native call.

**Coordinates.** `tileCoordinates` is the pure arithmetic of a mapping, one region per layer:

- A texture region is a box in tiles (`UseBox`), starting at the offset divided by the tile
  extent, of the tiles the extent spans (a partial last tile included), at subresource
  `mip + layer * mips`. The `UpdateTileMappings` order (x, then y, then z within a region, regions
  in order) matches the reference's.
- A mip tail is, per layer, one run of `NumTilesForPackedMips` tiles from x 0 of the first packed
  mip of that layer, without a box.
- A buffer range is one run of the tiles the range spans from the tile at `bufferOffset`, at
  subresource 0.
- The heap range starts at `heapOffset` divided by 64 KiB and advances by each layer's tile
  count. An unmap names no heap range: its flag is `D3D12_TILE_RANGE_FLAG_NULL` and its heap is
  null.

**Mapping.** `Queue::mapTiles` calls `core::mapTiles` first, with the queue's type, and then
groups the elements by resource and heap (null for an unmap) in the order each pair first appears.
Each group is one `UpdateTileMappings` with `D3D12_TILE_MAPPING_FLAG_NONE` and one range per
region, each range counted by its region's tiles; each call counts one `nativeSubmitCalls`.
`VU-rojo-sparse-0012` forbids two elements naming one tile, so grouping reorders nothing that
matters. Any queue type maps: the same call runs on graphics, compute and copy queues. The call is
ordered after the queue's earlier work and before its later work. It issues no barrier and no
discard; the first use of a newly mapped tile is ordered by the barrier `VU-rojo-sparse-0014`
requires of the caller. Destroying a reserved resource issues no native unmap; its tiles go with
the resource.

## Capture

The backend captures through PIX's programmatic GPU capture. With `DeviceDesc::enableCapture`,
`createDevice` calls `loadCapturer` before the device factory exists, so the capturer can attach to
it. `loadCapturer` lists `%ProgramFiles%\Microsoft PIX` (`FOLDERID_ProgramFiles`), keeps the
directories that hold `WinPixGpuCapturer.dll`, picks the one with the highest version by comparing
its runs of digits as numbers from left to right (`newestVersion`) and loads that file with
`LoadLibraryExW`, searching the default directories and its own. The module stays loaded for the
process: the capturer cannot be unloaded safely. A process that already holds it, a machine with
no PIX installed and a device created without `enableCapture` load nothing; a capturer that fails
to load logs a warning.

`Device::beginCapture` runs `core::beginCapture`, which refuses an empty path and a second capture.
A device created without `enableCapture` registers the hook `refuseCapture`, which returns
`Unsupported` naming `enableCapture` even when another device loaded the process-wide capturer. On a
device created with it, the hook `startCapture` resolves `BeginProgrammaticGpuCapture` from the
loaded module and calls it with the absolute form of the path, converted from UTF-8 to UTF-16. PIX's
`pix3_win.h` implements `PIXBeginCapture2` and `PIXEndCapture` as inline helpers over the capturer's
exports, so the backend calls the exports itself and links no PIX runtime. Without the capturer,
`beginCapture` returns `Unsupported` saying PIX is not loaded and `enableCapture` is needed; a path
that is not valid UTF-8 or cannot be made absolute is `Unsupported` too, since `ErrorCategory` has
no invalid-argument value. A capturer that refuses the capture returns an error of the refusal's
`HRESULT`, `DriverFailure` unless it is `E_OUTOFMEMORY`. `Device::endCapture` resolves
`EndProgrammaticGpuCapture` and logs an error when it fails. Each successful `beginCapture` and each
`endCapture` that reaches the capturer counts one `nativeHostCalls`.

A real capture needs PIX installed and hardware work; hosted CI has no PIX, so the cases cover
the search, the absent-capturer error and the device without `enableCapture`.

## FSR

FSR is the only upscaler the backend lists. `createDevice` loads the FidelityFX runtime once for
the device: `loadFidelityFx` checks for `amd_fidelityfx_loader_dx12.dll` in the executable's
directory, loads it with `LoadLibraryExW` (the loader's own imports are searched there and in the
default directories, never the current directory or `PATH`) and resolves `ffxCreateContext`,
`ffxDestroyContext`, `ffxConfigure`, `ffxQuery` and `ffxDispatch`. An absent loader is a supported
state and loads nothing silently; a loader that does not load or misses an export logs a warning
and leaves FSR unavailable. The module is freed once at `destroyDevice`, after the upscale
contexts have retired; the system counts references, so devices share one copy. RojoRHI commits no
SDK file: setup pins the headers and the two DLLs under `ThirdParty/fidelityfx/`, and the build
copies the DLLs beside an executable only when they are there
([the build record](d3d12-build.md)).

**Availability.** `capabilities.upscalers` is `{FSR}` when the loader loaded and an `ffxQuery`
with `ffxQueryDescGetVersions` for the upscale description and the device reports at least one
provider; otherwise it is empty. `UpscalerKind::Auto` resolves to FSR; DLSS, XeSS and MetalFX are
not listed, so `createUpscaler` refuses them with `Unsupported` naming the kind. When the runtime
did not load, a refusal of `Auto` or FSR adds that FSR needs `amd_fidelityfx_loader_dx12.dll`
beside the executable.

**Creation.** After `core::prepareUpscaler`, `ffxCreateContext` takes an
`ffxCreateContextDescUpscale` (the input as `maxRenderSize`, the output as `maxUpscaleSize`)
chained to `ffxCreateContextDescUpscaleVersion` (`FFX_UPSCALER_VERSION`) and
`ffxCreateBackendDX12Desc` (the device). `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION` is always set,
because every upscale may shrink the content; `HDR`, `ReversedDepth` and `AutoExposure` set
`..._HIGH_DYNAMIC_RANGE`, `..._DEPTH_INVERTED` and `..._AUTO_EXPOSURE`. A refusal maps
`FFX_API_RETURN_ERROR_MEMORY` to `OutOfMemory`, `FFX_API_RETURN_NO_PROVIDER` to `Unsupported` and
anything else to `DriverFailure`, naming the code. The context is the record's native object; the
pool's allocation counts one `nativeCreateCalls`. A FidelityFX context is no `ID3D12Object`, so it
takes no name.

**Lifetime.** The retire hook `retireUpscaler` calls `ffxDestroyContext`; a refusal logs and aborts
in every build. A Debug `destroyDevice` asserts that the device's last reference is gone, which
also checks that the version query and the contexts leave none behind.

**Thread safety.** The FidelityFX loader is one per process and documents no thread safety, so one
function-local mutex covers every `ffxQuery`, `ffxCreateContext` and `ffxDestroyContext` of every
device. It is a leaf.

**Recording.** `CommandList::upscale` (`D3D12Upscaler.cpp`) runs `core::upscale`, then fills one
`ffxDispatchDescUpscale` from the texture payloads without a lock. Colour, depth, motion vectors and
the optional exposure and reactive masks are inputs in `FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ`,
the state `Layout::ShaderRead` maps to; the output is a UAV in `..._UNORDERED_ACCESS`, the state of
`Layout::ShaderWrite`; a depth plane is `R32_FLOAT` and an sRGB output its linear format. The render
size is the content extent, or the input's when that is zero. The reference carries no frame time
or camera, so the dispatch passes 1000/60 ms, near 0.1, far 100, a vertical field of view of 1
radian and a metre factor of 1, the floor probe's values. `ffxDispatch` runs between the scope's
event and its timestamp pair; a result other than OK logs and aborts in every build. FidelityFX
issues its own barriers between the states above, so the backend adds none.

FidelityFX leaves the list with its own heaps, root signatures and pipeline, so
`resumeAfterVendorWork` sets the shader-visible heaps again at once and marks both root signatures
and roots, `b1`, the pipelines and the topology for the next use: the next dispatch costs four
native calls, the next `setPipeline` two and the next draw four. FidelityFX makes internal surfaces
at half the input extent: `createUpscaler` refuses an input under 2 texels per axis (`Unsupported`).

## Locks and counts

Locks the edges add, all leaves: `Presentation::mutex` guards the live and retired swapchain lists
and the presentation fence; it is held across the lazy `CreateFence` and the one `Signal` of a
window reuse, never across a `Present` or a wait. `SwapchainNative::mutex` guards one swapchain's
back-buffer payloads, held only around reads and writes, never across a wait or `Present`. The
FidelityFX mutex above is process-wide; the retire hook takes it under the upscaler pool's lock, so
the order is pool, then FidelityFX, and `createUpscaler` releases it before it allocates. The two
swapchain locks are never held together or while the FidelityFX mutex is taken. Recording takes no
lock ([threading](d3d12-backend.md#threading-and-lock-order)).

Counters follow the native call, as the rest of the backend does; reading a tile shape, querying
FSR and freeing swapchains at `destroyDevice` count nothing:

| Counter | Edge sites |
|---|---|
| `nativeSubmitCalls` | each `Present`; each `UpdateTileMappings`; the `Signal` of a window reuse |
| `nativeHostCalls` | each wait on the waitable object; the fence waits of `resize` and of a window reuse (`waitForFence` returns their number); each successful `beginCapture`; each `endCapture` that reaches the capturer |
| `nativeCreateCalls` | one per swapchain at creation (`core::prepareSwapchain`); one per swapchain freed at slot reuse or window reuse (`D3D12Swapchain.cpp`); the presentation fence; one per FSR context (the pool) |
| `descriptorWrites` | each back-buffer render-target view, at creation and at each resize |
| `cpuAllocations` | each swapchain and each image payload |
| `deferredRetired` | each swapchain freed at slot reuse or window reuse; each upscaler the core retires |

## Redistribution

A consumer that ships `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` with
its executable reproduces AMD's notice from `ThirdParty/fidelityfx/license.md`, the licence of the
pinned FidelityFX SDK release that `xmake setup` keeps there. The DLLs are AMD's signed binaries,
covered by that licence, not by RojoRHI's. RojoRHI commits no FidelityFX file: `xmake setup` fetches
the headers, the DLLs and the licence file and checks each against a pinned SHA-256.

## The T1 run on WARP

The edge cases are the `[d3d12]` files under `Tests/D3D12/` and the contract and conformance
files that cover the edges. [CI run 39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757) built Debug (debug layer and GPU-based
validation, whose messages abort the run) and Release on WARP and ran every contract file, every
death file in Debug and every T1 conformance file the build does not leave out. It is no T1
acceptance, which waits for a hardware adapter ([ADR 0009](
../decisions/0009-conformance-and-valid-usage.md)).

Debug: `abi` 10 cases (8 passed, 2 skipped), `contract` 578 (567, 11), `death` 555 (550, 3, and 2
that fail as expected), `conformance` 147 (136, 11). Release (no validation, no layer): `contract`
588 (485, 103) and `conformance` 152 (142, 10).

A third Debug configure, with the debug layer alone, runs the six files holding 64 upscale cases
WARP does not complete under GPU-based validation; all pass. No case is PC-only.

`xmake/targets.lua` leaves out `UpscalerTests.cpp` and `UpscaleConformanceTests.cpp` under GPU-based
validation, where the shared cases that wait for an upscale skip through
`test::requireUpscaleCompletion`, and, until #100 merges, the `AbiGraphics`, `Draw` and `Indirect`
conformance files, whose cases expect a base-including `vertexIndex`. Cases that skip print their
reasons: `maxDrawIndirectCount` is `UINT32_MAX`, so no `maxCount` lies above it
(`DrawDeathTests`, #109), and its records exceed `maxBufferSize` (`DrawTests`); the device packs no
mip tail for the sparse test texture (`SparseTests`, `SparseOrderTests`, `SparseOrderDeathTests`,
`SparseConformanceTests`, #102); the `TrackerDeathTests` case "a cleared range is in General" ends
the process in the layer on the clear's own barrier before the tracker judges the list;
`D3D12PipelineTests` skips a refusal the debug layer reports as an error message, and
`D3D12IndirectTests` a `maxCount` of 0, which a Debug build checks (`VU-rojo-indirect-0003`).

**Newly mapped tiles (#68).** Two `SparseConformanceTests.cpp` cases render into a newly mapped
tile of a render target and of a depth texture after the discard `VU-rojo-sparse-0014` requires.
A barrier from `General` in its place draws STATE_SETTING 1334 from the layer (layout `COMMON`,
expected `UNDEFINED`; run 39616320545, scratch branch), so the discard is what it needs.

**Not verified.** A real PIX capture; mip-tail paths; a `D32FloatS8` input or nonzero FSR jitter;
`DXGI_STATUS_OCCLUDED`; tearing; and anything that needs hardware.
