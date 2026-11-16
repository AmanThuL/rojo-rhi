//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Swapchain.h
/// @brief Declares the native state of a D3D12 swapchain, the presentation state of a device, and the present-mode seam.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "D3D12Descriptors.h"
#include "D3D12Textures.h"

#include <dxgi1_6.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
    struct SwapchainImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    /// How a swapchain of one `PresentMode` is created and presented: the sync interval and flags every `Present` takes, and
    /// the `DXGI_SWAP_CHAIN_FLAG` bits creation and `ResizeBuffers` take.
    struct PresentFlags
    {
        UINT interval = 1;       ///< The sync interval of `IDXGISwapChain::Present`.
        UINT flags = 0;          ///< The `DXGI_PRESENT` flags of `IDXGISwapChain::Present`.
        UINT swapchainFlags = 0; ///< The `DXGI_SWAP_CHAIN_FLAG` bits of creation and of every `ResizeBuffers`.
    };

    /// The native objects of one swapchain, held until the swapchain is freed, once `Device::beginFrame` has waited for the
    /// frame that destroyed it, at the creation of a new swapchain on its window, or at device destruction. Each image's
    /// payload is the payload of the texture `Swapchain::acquire` returns for it: it borrows the back buffer and its
    /// render-target view, and the texture's retirement releases nothing of it.
    struct SwapchainNative
    {
        Com<IDXGISwapChain3> swapchain;                      ///< The flip-model swapchain on the window.
        HANDLE waitable = nullptr;                           ///< The frame-latency waitable object, closed when freed.
        Com<ID3D12DescriptorHeap> targets;                   ///< The CPU-only heap of one render-target view per back buffer.
        std::vector<std::unique_ptr<TexturePayload>> images; ///< Per back buffer, its payload: one reference to the buffer.
        PresentFlags present;                                ///< The present and creation flags of the swapchain's mode.
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;            ///< The format of every back buffer.
        HRESULT presented = S_OK;                            ///< The last `Present`'s result, which the D3D12 tests read for occlusion.
        std::mutex mutex;                                    ///< Guards the back buffers of `images`; a leaf, never held across a wait.
    };

    /// A destroyed swapchain and the last frame that may have presented it: the open frame when it was destroyed, or the last
    /// ended one between frames.
    struct RetiredSwapchain
    {
        uint64_t frame = 0;                       ///< The frame whose completion frees the swapchain.
        std::unique_ptr<SwapchainImpl> swapchain; ///< The destroyed swapchain.
    };

    /// The swapchains of one device: the live ones, and the destroyed ones, each freed once `Device::beginFrame` has waited for
    /// its frame. A window takes one swapchain at a time, so a new swapchain on the window of a destroyed one frees that one
    /// first, once the graphics queue signals `fence` after its presents.
    struct Presentation
    {
        std::mutex mutex;                                       ///< Guards every member; a leaf.
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains; ///< The live swapchains.
        std::vector<RetiredSwapchain> retired;                  ///< The destroyed swapchains, in the order they were destroyed.
        Com<ID3D12Fence> fence;                                 ///< Signalled before a window is reused.
        uint64_t signalled = 0;                                 ///< The last value `fence` was signalled with.
    };

    /// Returns the present and creation flags of a swapchain of `mode` on a system that reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING`
    /// when `tearingSupported`: `Fifo` presents with interval 1, `Mailbox` with interval 0, `Immediate` with interval 0 and
    /// `DXGI_PRESENT_ALLOW_TEARING` on a swapchain created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`; every mode creates the
    /// swapchain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. `Immediate` without tearing is `Unsupported`.
    Result<PresentFlags> presentFlags(PresentMode mode, bool tearingSupported);

    /// Returns the DXGI color space chapter 11 maps `colorSpace` to.
    DXGI_COLOR_SPACE_TYPE colorSpaceOf(ColorSpace colorSpace);

    /// Returns true when `result`, returned by `IDXGISwapChain::Present`, is a successful present: `S_OK`, or
    /// `DXGI_STATUS_OCCLUDED` for a window the display does not show.
    bool presentSucceeded(HRESULT result);

    /// Frees the destroyed swapchains of `impl` whose frame is `waited` or earlier, once `Device::beginFrame` has waited for
    /// frame `waited`.
    void freePassedSwapchains(DeviceImpl& impl, uint64_t waited);

    /// Frees every swapchain of `impl`, live or destroyed, at device destruction, once no queue holds work, and the
    /// presentation fence.
    void releaseSwapchains(DeviceImpl& impl);
} // namespace rojoRHI::d3d12
