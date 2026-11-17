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
    struct PresentFlags
    {
        UINT interval = 1;
        UINT flags = 0;
        UINT swapchainFlags = 0; // Creation and every `ResizeBuffers` take them.
    };

    // Held until the swapchain is freed: once `Device::beginFrame` has waited for the frame that destroyed it, at the
    // creation of a new swapchain on its window, or at device destruction. Each image's payload is that of the texture
    // `Swapchain::acquire` returns for it: it borrows the back buffer and its render-target view, and the texture's
    // retirement releases nothing of it.
    struct SwapchainNative
    {
        Com<IDXGISwapChain3> swapchain;                      // Flip model.
        HANDLE waitable = nullptr;                           // Closed when freed.
        Com<ID3D12DescriptorHeap> targets;                   // CPU-only: one render-target view per back buffer.
        std::vector<std::unique_ptr<TexturePayload>> images; // One reference to each back buffer.
        PresentFlags present;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        HRESULT presented = S_OK; // The D3D12 tests read it for occlusion.
        std::mutex mutex;         // Guards the back buffers of `images`; a leaf, never held across a wait.
    };

    // The frame is the open frame when it was destroyed, or the last ended one between frames.
    struct RetiredSwapchain
    {
        uint64_t frame = 0;
        std::unique_ptr<SwapchainImpl> swapchain;
    };

    // Each destroyed swapchain is freed once `Device::beginFrame` has waited for its frame. A window takes one
    // swapchain at a time, so a new swapchain on a destroyed one's window frees that one first, once the graphics queue
    // signals `fence` after its presents.
    struct Presentation
    {
        std::mutex mutex; // Guards every member; a leaf.
        std::vector<std::unique_ptr<SwapchainImpl>> swapchains;
        std::vector<RetiredSwapchain> retired; // In the order they were destroyed.
        Com<ID3D12Fence> fence;                // Signalled before a window is reused.
        uint64_t signalled = 0;
    };

    // `Fifo` presents with interval 1, `Mailbox` with 0, `Immediate` with 0 and DXGI_PRESENT_ALLOW_TEARING on a
    // swapchain created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; every mode creates the swapchain with
    // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT. `Immediate` without tearing is `Unsupported`.
    Result<PresentFlags> presentFlags(PresentMode mode, bool tearingSupported);

    // As chapter 11 maps it.
    DXGI_COLOR_SPACE_TYPE colorSpaceOf(ColorSpace colorSpace);

    // S_OK, or DXGI_STATUS_OCCLUDED for a window the display does not show.
    bool presentSucceeded(HRESULT result);

    // Once `Device::beginFrame` has waited for frame `waited`.
    void freePassedSwapchains(DeviceImpl& impl, uint64_t waited);

    // At device destruction, once no queue holds work; the presentation fence goes too.
    void releaseSwapchains(DeviceImpl& impl);
} // namespace rojoRHI::d3d12
