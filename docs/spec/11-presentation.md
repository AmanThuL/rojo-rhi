# 11. Presentation

**Status**: Accepted
**API version**: 0.1.0

A swapchain owns the images one native surface displays. Windowing stays the consumer's business:
RojoRHI never creates a window or a layer, and the surface arrives as an opaque pointer. While
acquired, an image is an ordinary `Texture` handle, ordered by the barriers of
[synchronization](09-synchronization.md) and submitted through the queues of
[queues and frames](10-queues-and-frames.md).

One frame of presentation runs in this order:

1. `Swapchain::acquire`, inside an open frame, returns the next image with undefined contents.
2. The caller transitions the image from `Layout::Undefined` to `Layout::ColorAttachment`, renders
   into it, and transitions it to `Layout::Present`, in command lists it submits to the graphics
   queue.
3. `Queue::present` on the graphics queue hands the image to the display before
   `Device::endFrame` ([device](02-device.md)).

Presentation setup, meaning creation, resizing and release, sits outside the frame loop and may
wait. The image extent is fixed between those calls: a new swapchain takes the surface's extent at
creation (the layer's `drawableSize` on Metal 4, the window's client extent on the planned D3D12
backend), and only `Swapchain::resize` changes it afterwards; `acquire` never resizes. Because the
swapchain owns the extent, the consumer does not set the layer's `drawableSize` itself once the
swapchain exists. A swapchain is released with `Device::destroy` ([device](02-device.md)),
deferred like a resource; its native presentation objects go once its last present retires. The
native surface must outlive the swapchain: it may be destroyed only after `Device::destroy` on
the swapchain and the retirement of every frame in flight at that call.

### `ColorSpace`
```cpp
enum class ColorSpace : uint8_t { SRGB, ExtendedLinear, HDR10 };
```
How the display interprets the values written to a swapchain image. Each color space admits only
the formats listed, named in [formats and limits](15-formats-and-limits.md).

| Enumerator | Meaning | Formats |
|---|---|---|
| `SRGB` | Standard dynamic range, sRGB primaries and transfer; the shader writes sRGB-encoded values | `BGRA8Unorm` |
| `ExtendedLinear` | Extended dynamic range (EDR): linear, sRGB primaries, values above 1.0 brighter than SDR white | `RGBA16Float` |
| `HDR10` | BT.2020 primaries, SMPTE ST 2084 (PQ) transfer | `RGB10A2Unorm` |

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `SRGB` | `CA::MetalLayer::setColorspace` with `kCGColorSpaceSRGB` | `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` | `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` |
| `ExtendedLinear` | `kCGColorSpaceExtendedLinearSRGB` with the layer's `wantsExtendedDynamicRangeContent` (metal-cpp wrapper unverified) | `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (scRGB) | `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` |
| `HDR10` | `kCGColorSpaceITUR_2100_PQ` with the layer's `wantsExtendedDynamicRangeContent` (metal-cpp wrapper unverified) | `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` | `VK_COLOR_SPACE_HDR10_ST2084_EXT` |
| Cost | native | native: set through `SetColorSpace1` after `CheckColorSpaceSupport` | native: the non-sRGB spaces need `VK_EXT_swapchain_colorspace` |

### `PresentMode`
```cpp
enum class PresentMode : uint8_t { Fifo, Mailbox, Immediate };
```
When a presented image reaches the display.

| Enumerator | Meaning |
|---|---|
| `Fifo` | Each image is shown for at least one vertical blank, in present order; never tears |
| `Mailbox` | Shown at the next vertical blank; a newer present replaces a queued one; does not tear, except that on Metal 4 a layer presented direct to the display may tear |
| `Immediate` | Shown as soon as possible; may tear |

A mode the surface or display cannot provide makes `Device::createSwapchain` return an error;
`Fifo` is always available.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Fifo` | `CA::MetalLayer::setDisplaySyncEnabled(true)` | `Present(1, 0)` | `VK_PRESENT_MODE_FIFO_KHR` |
| `Mailbox` | `CA::MetalLayer::setDisplaySyncEnabled(false)` while the window server composites the layer | `Present(0, 0)` on the flip model | `VK_PRESENT_MODE_MAILBOX_KHR` |
| `Immediate` | `CA::MetalLayer::setDisplaySyncEnabled(false)` | `Present(0, DXGI_PRESENT_ALLOW_TEARING)`, swapchain created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` | `VK_PRESENT_MODE_IMMEDIATE_KHR` |
| Cost | native: `Mailbox` and `Immediate` share one setting, and a layer presented direct to the display can tear in `Mailbox` | native: `Immediate` needs `DXGI_FEATURE_PRESENT_ALLOW_TEARING` | native: `Mailbox` and `Immediate` are optional per surface |

### `SwapchainDesc`
```cpp
struct SwapchainDesc {
    void* nativeSurface = nullptr;            // CA::MetalLayer* on Metal 4, HWND on D3D12
    Format format = Format::BGRA8Unorm;
    ColorSpace colorSpace = ColorSpace::SRGB;
    uint32_t imageCount = 3;
    PresentMode mode = PresentMode::Fifo;
    uint32_t maxLatency = 2;
    std::string_view label;
};
```

| Field | Meaning |
|---|---|
| `nativeSurface` | The consumer-owned surface the images are displayed on |
| `format` | Format of every image; forms a pair with `colorSpace` |
| `colorSpace` | How the display interprets image values |
| `imageCount` | Images the swapchain rotates through |
| `mode` | When a presented image reaches the display |
| `maxLatency` | Most presents of this swapchain that may be queued and not yet displayed; `acquire` waits while the limit is reached |
| `label` | Diagnostic label, copied at creation; defaults to `rojorhi.swapchain.unnamed` |

#### Valid usage
- `VU-rojo-present-0001` `nativeSurface` is not null.
- `VU-rojo-present-0002` `nativeSurface` is the surface type of the compiled backend: a
  `CA::MetalLayer*` on Metal 4, an `HWND` on D3D12.
- `VU-rojo-present-0003` `format` is the format `ColorSpace` lists for `colorSpace`.
- `VU-rojo-present-0004` `imageCount` is 2 or 3.
- `VU-rojo-present-0005` `maxLatency` is at least 1 and at most `imageCount`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Surface | `CA::MetalLayer` | `HWND` | `VkSurfaceKHR` |
| Image count | `CA::MetalLayer::setMaximumDrawableCount` | `BufferCount` | `minImageCount` |
| Latency | the backend counts pending presents through drawable presented handlers | `SetMaximumFrameLatency` | `vkWaitForPresentKHR`, which needs `VK_KHR_present_wait` |
| Cost | cheap: `maxLatency` is enforced by the backend, since a layer has no latency setting | native | cheap: `maxLatency` is enforced by the backend through present waits |

### `Device::createSwapchain`
```cpp
Result<Swapchain> Device::createSwapchain(const SwapchainDesc& desc);
```
Binds a swapchain to `desc.nativeSurface` for the graphics queue, at the surface's current extent.
Creation returns an error when the color space or present mode is unavailable for the surface or
display, or when the platform refuses the surface. Creation is presentation setup and may wait:
when the surface's previous swapchain was destroyed but its last present has not retired, the call
waits for that retirement, because neither DXGI nor Vulkan binds two swapchains to one window.
Creation is free-threaded.

#### Valid usage
- `VU-rojo-present-0006` `desc.nativeSurface` is not the surface of another swapchain of this
  device that has not been destroyed.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | configure the `CA::MetalLayer`: `setDevice`, `setPixelFormat`, `setColorspace`, `setMaximumDrawableCount`, `setDisplaySyncEnabled`, `setFramebufferOnly`, and `wantsExtendedDynamicRangeContent` (metal-cpp wrapper unverified) | `IDXGIFactory2::CreateSwapChainForHwnd` with `DXGI_SWAP_EFFECT_FLIP_DISCARD` and `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, then `SetMaximumFrameLatency` and `SetColorSpace1` | `vkCreateSwapchainKHR` |
| Cost | native | native | native |

### `Swapchain::resize`
```cpp
void Swapchain::resize(Extent2D extent);
```
Sets the extent of every image of the swapchain to `extent`
([`Extent2D`](01-fundamentals.md#extent2d)). Resizing is presentation setup outside the frame
loop: the call waits until the graphics queue has finished all work that references this
swapchain's images, then replaces them. The next `acquire`
returns an image of the new extent. A consumer resizes when its window changes size and skips
presentation while the window has a zero extent.

#### Valid usage
- `VU-rojo-present-0007` No frame is open: the call comes after `Device::endFrame` and before the
  next `Device::beginFrame`.
- `VU-rojo-present-0008` No image of this swapchain is acquired and not yet presented.
- `VU-rojo-present-0009` `extent.width` and `extent.height` are greater than zero.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `CA::MetalLayer::setDrawableSize` after the swapchain's drawables retire | `IDXGISwapChain::ResizeBuffers` after the swapchain's images retire | swapchain re-creation with `oldSwapchain` |
| Cost | native | native | native |

### `Swapchain::acquire`
```cpp
Texture Swapchain::acquire();
```
Returns the next image of the swapchain, labelled after the swapchain, with undefined contents. The
call first waits while `maxLatency` presents of this swapchain are pending, then until an image is
free. These are the only waits inside the frame loop besides frame pacing in `Device::beginFrame`
([device](02-device.md)). It returns a null `Texture` when no image became free within one second; a
null result means the frame presents nothing to this swapchain. The call never changes the extent.

The caller's first barrier on the image names `Layout::Undefined` as its old layout, on the first
acquire of an image and on every later one. That is the one rule every backend can honour: a
Vulkan image arrives in `VK_IMAGE_LAYOUT_UNDEFINED` on its first acquire and in
`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` afterwards, and a barrier from undefined is valid from either,
while a D3D12 back buffer is in `D3D12_BARRIER_LAYOUT_PRESENT` and Metal 4 has no layouts.

The image may be used only as a color attachment and in texture barriers. Its handle stays valid
until `Queue::present` on this swapchain; afterwards it is stale and fails the generation check.
On Metal 4 the call also makes the graphics queue wait for the drawable, so every later submission
to that queue is ordered after the display releases it.

#### Valid usage
- `VU-rojo-present-0010` A frame is open: the call comes between `Device::beginFrame` and
  `Device::endFrame`.
- `VU-rojo-present-0011` No image of this swapchain is acquired and not yet presented.
- `VU-rojo-present-0012` The returned texture is used only as a color attachment and in texture
  barriers.
- `VU-rojo-present-0013` The returned texture is never passed to `Device::destroy`.
- `VU-rojo-present-0014` The first texture barrier on the returned texture after the call has
  `oldLayout` `Layout::Undefined`. Checked by the validation tracker.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `CA::MetalLayer::nextDrawable`, then `MTL4::CommandQueue::wait` with the drawable on the graphics queue | wait on `GetFrameLatencyWaitableObject`, then `GetCurrentBackBufferIndex` | `vkAcquireNextImageKHR` signalling a binary semaphore |
| Cost | native | native | native: the semaphore joins the next graphics submission |

### `Queue::present`
```cpp
void Queue::present(Swapchain swapchain);
```
Hands the swapchain's acquired image to the display once all work submitted earlier to this queue
completes, following the swapchain's `PresentMode`. The call does not wait. The caller transitions
the image back to `Layout::Present` first, in work submitted to this queue before the call.
Presentation is serialized with submission on the queue.

#### Valid usage
- `VU-rojo-present-0015` The queue is the graphics queue.
- `VU-rojo-present-0016` The swapchain holds a non-null image acquired in this frame and not yet
  presented.
- `VU-rojo-present-0017` The call comes before the `Device::endFrame` that closes the frame of
  the acquire.
- `VU-rojo-present-0018` The last texture barrier on the image in work submitted to this queue
  before the call has `newLayout` `Layout::Present`. Checked by the validation tracker.
- `VU-rojo-present-0019` No command list submitted after the call references the image. Checked by
  the validation tracker.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CommandQueue::signalDrawable`, then `MTL::Drawable::present` | `IDXGISwapChain::Present` with the mode's sync interval and flags | `vkQueuePresentKHR` waiting on a binary semaphore the last submission signals |
| Cost | native | native | native |
