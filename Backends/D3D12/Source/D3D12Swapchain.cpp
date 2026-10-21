//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Swapchain.cpp
/// @brief Implements the swapchain entries of the D3D12 backend and `Queue::present` on DXGI flip-model swapchains.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Swapchain.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Swapchains.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12Fences.h"
#include "D3D12Formats.h"
#include "D3D12Textures.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        /// How long `Swapchain::acquire` waits on the frame-latency waitable object, in milliseconds: chapter 11's one second.
        constexpr DWORD kAcquireDeadlineMs = 1'000;

        //==============================================================================================================
        /// Returns the device that created `swapchain`.
        DeviceImpl& deviceOf(const SwapchainImpl& swapchain)
        {
            return *static_cast<DeviceImpl*>(swapchain.core.device->retireHooks().context);
        }

        //==============================================================================================================
        /// Reports the removal of `impl`'s device, found by `entry`, then logs and aborts, in every build, when the device was
        /// removed; does nothing otherwise.
        void abortIfRemoved(DeviceImpl& impl, std::string_view entry)
        {
            if (const HRESULT reason = impl.device()->GetDeviceRemovedReason(); reason != S_OK)
            {
                d3d12::reportRemoval(impl, entry);
                ROJORHI_LOG_ERROR("{} found the D3D12 device removed with reason 0x{:08X}", entry, d3d12::hresultBits(reason));
                std::abort();
            }
        }

        //==============================================================================================================
        /// Returns the name of `colorSpace` and of the DXGI color space it maps to, for messages.
        std::string_view colorSpaceName(ColorSpace colorSpace)
        {
            switch (colorSpace)
            {
            case ColorSpace::SRGB: return "ColorSpace::SRGB (DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)";
            case ColorSpace::ExtendedLinear: return "ColorSpace::ExtendedLinear (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)";
            case ColorSpace::HDR10: return "ColorSpace::HDR10 (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)";
            }
            return "a value that is no ColorSpace";
        }

        //==============================================================================================================
        /// Returns the frame whose completion frees a swapchain of `impl` destroyed now: the open frame, or the last ended one
        /// between frames. Presents and the lists that use acquired images come only inside a frame, so none of the
        /// swapchain's can come later, and they are complete once `Device::beginFrame` has waited for that frame. A
        /// `Device::beginFrame` running meanwhile opens its frame before it frees, so a swapchain destroyed during it takes the
        /// new frame, or the one before it, and either is later than the frame that call waited for.
        uint64_t retireFrame(const DeviceImpl& impl)
        {
            return impl.core.frameNumber();
        }

        //==============================================================================================================
        /// Fetches every back buffer of `native`'s swapchain, whose core is `state`, with `GetBuffer`, names it
        /// `<label>.image<i>` and writes its render-target view into the swapchain's heap, each write one
        /// `descriptorWrites`; the image's payload then holds the one reference to its buffer and the core's extent, which
        /// render passes read. The caller holds `native.mutex`. Returns the error of a refused `GetBuffer`.
        Result<void> fetchImages(DeviceImpl& impl, d3d12::SwapchainNative& native, const core::SwapchainCore& state)
        {
            const std::string& label = state.label;
            for (size_t i = 0; i < native.images.size(); ++i)
            {
                d3d12::TexturePayload& image = *native.images[i];
                image.width = state.extent.width;
                image.height = state.extent.height;
                ID3D12Resource* buffer = nullptr;
                if (const HRESULT got = native.swapchain->GetBuffer(static_cast<UINT>(i), IID_PPV_ARGS(&buffer)); FAILED(got))
                {
                    return std::unexpected(d3d12::failure(impl, got, std::format("IDXGISwapChain::GetBuffer for image {} of swapchain \"{}\"", i, label)));
                }
                image.resource = buffer;
                d3d12::setName(buffer, std::format("{}.image{}", label, i));
                D3D12_RENDER_TARGET_VIEW_DESC view = {};
                view.Format = native.format;
                view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipSlice = 0;
                view.Texture2D.PlaneSlice = 0;
                impl.device()->CreateRenderTargetView(buffer, &view, image.targetStart);
                core::DeviceCounts::add(impl.core.counts().descriptorWrites);
            }
            return {};
        }

        //==============================================================================================================
        /// Releases every back-buffer reference `native` holds; the render-target views stay in the heap until fetched again.
        /// The caller holds `native.mutex`.
        void dropImages(d3d12::SwapchainNative& native)
        {
            for (const std::unique_ptr<d3d12::TexturePayload>& image : native.images)
            {
                if (image->resource != nullptr)
                {
                    image->resource->Release();
                    image->resource = nullptr;
                }
            }
        }

        //==============================================================================================================
        /// Releases the native objects of `swapchain`, made in full or in part: its back buffers, its render-target heap, its
        /// waitable object and the DXGI swapchain. No queue holds work that uses them. The texture records of its images may
        /// still wait for their frame; their retirement never reads the payloads freed here.
        void freeSwapchain(SwapchainImpl& swapchain)
        {
            d3d12::SwapchainNative& native = swapchain.native;
            {
                std::lock_guard lock(native.mutex);
                dropImages(native);
                native.images.clear();
            }
            native.targets.reset();
            if (native.waitable != nullptr)
            {
                CloseHandle(native.waitable);
                native.waitable = nullptr;
            }
            native.swapchain.reset();
        }

        //==============================================================================================================
        /// Frees `freed`, swapchains destroyed on `impl` whose presents have completed, counting each as one retired deferred
        /// destruction and one native release.
        void freeRetired(DeviceImpl& impl, std::vector<std::unique_ptr<SwapchainImpl>>& freed)
        {
            if (freed.empty())
            {
                return;
            }
            // A swapchain is no pool record, so the core's retire loop cannot see it: this is the one place that does.
            core::DeviceCounts::add(impl.core.counts().deferredRetired, freed.size());
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, freed.size());
            for (const std::unique_ptr<SwapchainImpl>& swapchain : freed)
            {
                freeSwapchain(*swapchain);
            }
            freed.clear();
        }

        //==============================================================================================================
        /// Creates the presentation fence of `impl` on first use, one counted `nativeCreateCalls`. The caller holds
        /// `presentation.mutex`.
        Result<void> ensureFence(DeviceImpl& impl)
        {
            d3d12::Presentation& presentation = impl.presentation;
            if (presentation.fence)
            {
                return {};
            }
            if (const HRESULT made = impl.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(presentation.fence.put())); FAILED(made))
            {
                return std::unexpected(d3d12::failure(impl, made, "ID3D12Device::CreateFence for \"rojorhi.presentation\""));
            }
            d3d12::setName(presentation.fence.get(), "rojorhi.presentation");
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            return {};
        }

        //==============================================================================================================
        /// Frees a swapchain of `impl` destroyed on the window `surface` that `Device::beginFrame` has not freed yet, since DXGI
        /// binds one swapchain to a window: the graphics queue signals the presentation fence after its presents, one counted
        /// `nativeSubmitCalls`, and the call waits for the fence, counting its host calls. Does nothing when no destroyed
        /// swapchain holds the window. Returns the error of a device found removed while it waits.
        Result<void> reclaimSurface(DeviceImpl& impl, const void* surface)
        {
            d3d12::Presentation& presentation = impl.presentation;
            std::vector<std::unique_ptr<SwapchainImpl>> freed;
            uint64_t value = 0;
            {
                std::lock_guard lock(presentation.mutex);
                const auto found = std::find_if(presentation.retired.begin(), presentation.retired.end(), [&](const d3d12::RetiredSwapchain& retired) {
                    return retired.swapchain->core.surface == surface;
                });
                if (found == presentation.retired.end())
                {
                    return {};
                }
                if (Result<void> made = ensureFence(impl); !made)
                {
                    // The swapchain stays in the list, which frees it as usual.
                    return made;
                }
                freed.push_back(std::move(found->swapchain));
                presentation.retired.erase(found);
                // Signalled under the lock, so the values reach the queue in the order they are taken. The caller's presents on
                // the destroyed swapchain came before its destruction, and so before this signal.
                value = ++presentation.signalled;
                ID3D12CommandQueue* graphics = impl.queues[static_cast<size_t>(QueueType::Graphics)].native.get();
                d3d12::requireSucceeded(impl, graphics->Signal(presentation.fence.get(), value), "ID3D12CommandQueue::Signal of the presentation fence");
                core::DeviceCounts::add(impl.core.counts().nativeSubmitCalls);
            }
            const d3d12::FenceWait waited = d3d12::waitForFence(presentation.fence.get(), value, UINT64_MAX, "the presentation fence");
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls, waited.hostCalls);
            // A removed device runs nothing more, so the swapchain is freed either way.
            freeRetired(impl, freed);
            if (waited.completed == UINT64_MAX)
            {
                if (const HRESULT reason = impl.device()->GetDeviceRemovedReason(); FAILED(reason))
                {
                    return std::unexpected(d3d12::failure(impl, reason, "the wait for a destroyed swapchain's presents in Device::createSwapchain"));
                }
            }
            return {};
        }

        //==============================================================================================================
        /// Returns whether the system reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING` to `impl`'s DXGI factory; a query the factory
        /// refuses reports none.
        bool systemAllowsTearing(DeviceImpl& impl)
        {
            BOOL allowed = FALSE;
            const HRESULT checked = impl.loaded.dxgi->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowed, static_cast<UINT>(sizeof(allowed)));
            return SUCCEEDED(checked) && allowed != FALSE;
        }

        //==============================================================================================================
        /// Makes the native objects of `swapchain`, whose core holds the checked description `desc`, on the graphics queue of
        /// `impl`: the DXGI swapchain at the window's client extent, which becomes the core's extent, its color space, frame
        /// latency and window association, its waitable object, its render-target heap and one payload per back buffer. A
        /// mode or color space the system or output refuses is `Unsupported`; any other refusal is the native call's error.
        /// Whatever was made before a failure stays in `swapchain.native` for `freeSwapchain`.
        Result<void> createNative(DeviceImpl& impl, SwapchainImpl& swapchain, const SwapchainDesc& desc)
        {
            const std::string& label = swapchain.core.label;
            d3d12::SwapchainNative& native = swapchain.native;
            if (Result<void> reclaimed = reclaimSurface(impl, desc.nativeSurface); !reclaimed)
            {
                return reclaimed;
            }
            const bool tearing = desc.mode == PresentMode::Immediate && systemAllowsTearing(impl);
            Result<d3d12::PresentFlags> flags = d3d12::presentFlags(desc.mode, tearing);
            if (!flags)
            {
                return std::unexpected(std::move(flags.error()));
            }
            native.present = *flags;
            native.format = d3d12::dxgiFormat(desc.format);
            const HWND window = static_cast<HWND>(desc.nativeSurface);
            DXGI_SWAP_CHAIN_DESC1 chain = {};
            // A zero extent takes the window's client extent.
            chain.Width = 0;
            chain.Height = 0;
            chain.Format = native.format;
            chain.Stereo = FALSE;
            chain.SampleDesc.Count = 1;
            chain.SampleDesc.Quality = 0;
            chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            chain.BufferCount = desc.imageCount;
            chain.Scaling = DXGI_SCALING_STRETCH;
            chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            chain.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            chain.Flags = native.present.swapchainFlags;
            ID3D12CommandQueue* graphics = impl.queues[static_cast<size_t>(QueueType::Graphics)].native.get();
            d3d12::Com<IDXGISwapChain1> created;
            if (const HRESULT made = impl.loaded.dxgi->CreateSwapChainForHwnd(graphics, window, &chain, nullptr, nullptr, created.put()); FAILED(made))
            {
                return std::unexpected(d3d12::failure(impl, made, std::format("IDXGIFactory2::CreateSwapChainForHwnd for swapchain \"{}\"", label)));
            }
            if (const HRESULT queried = created->QueryInterface(IID_PPV_ARGS(native.swapchain.put())); FAILED(queried))
            {
                return std::unexpected(d3d12::failure(impl, queried, std::format("QueryInterface for IDXGISwapChain3 of swapchain \"{}\"", label)));
            }
            const DXGI_COLOR_SPACE_TYPE space = d3d12::colorSpaceOf(desc.colorSpace);
            UINT support = 0;
            if (const HRESULT checked = native.swapchain->CheckColorSpaceSupport(space, &support); FAILED(checked))
            {
                return std::unexpected(d3d12::failure(impl, checked, std::format("IDXGISwapChain3::CheckColorSpaceSupport for swapchain \"{}\"", label)));
            }
            if ((support & static_cast<UINT>(DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) == 0)
            {
                return std::unexpected(
                    Error{ ErrorCategory::Unsupported,
                           std::format("{} is not supported for presentation on the window of swapchain \"{}\"", colorSpaceName(desc.colorSpace), label) }
                );
            }
            if (const HRESULT set = native.swapchain->SetColorSpace1(space); FAILED(set))
            {
                return std::unexpected(d3d12::failure(impl, set, std::format("IDXGISwapChain3::SetColorSpace1 for swapchain \"{}\"", label)));
            }
            if (const HRESULT set = native.swapchain->SetMaximumFrameLatency(desc.maxLatency); FAILED(set))
            {
                return std::unexpected(d3d12::failure(impl, set, std::format("IDXGISwapChain2::SetMaximumFrameLatency for swapchain \"{}\"", label)));
            }
            if (const HRESULT associated = impl.loaded.dxgi->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER); FAILED(associated))
            {
                return std::unexpected(d3d12::failure(impl, associated, std::format("IDXGIFactory::MakeWindowAssociation for swapchain \"{}\"", label)));
            }
            DXGI_SWAP_CHAIN_DESC1 actual = {};
            if (const HRESULT described = native.swapchain->GetDesc1(&actual); FAILED(described))
            {
                return std::unexpected(d3d12::failure(impl, described, std::format("IDXGISwapChain1::GetDesc1 for swapchain \"{}\"", label)));
            }
            swapchain.core.extent = { actual.Width, actual.Height };
            native.waitable = native.swapchain->GetFrameLatencyWaitableObject();
            if (native.waitable == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure,
                           std::format("IDXGISwapChain2::GetFrameLatencyWaitableObject returned none for swapchain \"{}\"", label) }
                );
            }
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = desc.imageCount;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            heapDesc.NodeMask = 0;
            if (const HRESULT heaped = impl.device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(native.targets.put())); FAILED(heaped))
            {
                return std::unexpected(d3d12::failure(impl, heaped, std::format("ID3D12Device::CreateDescriptorHeap for {}.rtv", label)));
            }
            d3d12::setName(native.targets.get(), std::format("{}.rtv", label));
            const D3D12_CPU_DESCRIPTOR_HANDLE start = native.targets->GetCPUDescriptorHandleForHeapStart();
            const UINT increment = impl.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            std::lock_guard lock(native.mutex);
            for (uint32_t i = 0; i < desc.imageCount; ++i)
            {
                auto image = std::make_unique<d3d12::TexturePayload>();
                core::DeviceCounts::add(impl.core.counts().cpuAllocations);
                image->targets = native.targets;
                image->targetCount = 1;
                image->resourceFormat = native.format;
                image->format = swapchain.core.format;
                image->usage = TextureUsage::RenderTarget;
                image->targetStart = D3D12_CPU_DESCRIPTOR_HANDLE{ start.ptr + SIZE_T{ i } * increment };
                image->targetIncrement = increment;
                native.images.push_back(std::move(image));
            }
            return fetchImages(impl, native, swapchain.core);
        }

        //==============================================================================================================
        /// Files the destroyed `swapchain` of `impl` with its `retireFrame`, which `Device::beginFrame` frees once it has waited
        /// for that frame.
        void retireSwapchain(DeviceImpl& impl, SwapchainImpl& swapchain)
        {
            d3d12::Presentation& presentation = impl.presentation;
            // The frame hook frees from the same list under the same lock.
            std::lock_guard lock(presentation.mutex);
            const auto found =
                std::find_if(presentation.swapchains.begin(), presentation.swapchains.end(), [&](const auto& live) { return live.get() == &swapchain; });
            ROJORHI_INTERNAL_ASSERT(found != presentation.swapchains.end(), "a swapchain that passed the device check is not in the device's list");
            presentation.retired.push_back(d3d12::RetiredSwapchain{ retireFrame(impl), std::move(*found) });
            presentation.swapchains.erase(found);
        }
    } // namespace

    namespace core {
        //==============================================================================================================
        SwapchainCore& coreOf(Swapchain swapchain)
        {
            return SwapchainImpl::of(swapchain).core;
        }
    } // namespace core

    namespace d3d12 {
        //==============================================================================================================
        Result<PresentFlags> presentFlags(PresentMode mode, bool tearingSupported)
        {
            const auto waitable = static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            switch (mode)
            {
            case PresentMode::Fifo: return PresentFlags{ 1, 0, waitable };
            case PresentMode::Mailbox: return PresentFlags{ 0, 0, waitable };
            case PresentMode::Immediate:
                if (!tearingSupported)
                {
                    return std::unexpected(
                        Error{ ErrorCategory::Unsupported, "PresentMode::Immediate needs DXGI_FEATURE_PRESENT_ALLOW_TEARING, which the system does not report" }
                    );
                }
                return PresentFlags{ 0, static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING), waitable | static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) };
            }
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("{} is no PresentMode", static_cast<int>(mode)) });
        }

        //==============================================================================================================
        DXGI_COLOR_SPACE_TYPE colorSpaceOf(ColorSpace colorSpace)
        {
            switch (colorSpace)
            {
            case ColorSpace::SRGB: return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            case ColorSpace::ExtendedLinear: return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            case ColorSpace::HDR10: return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }

        //==============================================================================================================
        bool presentSucceeded(HRESULT result)
        {
            return result == S_OK || result == DXGI_STATUS_OCCLUDED;
        }

        //==============================================================================================================
        void freePassedSwapchains(DeviceImpl& impl, uint64_t waited)
        {
            Presentation& presentation = impl.presentation;
            std::vector<std::unique_ptr<SwapchainImpl>> freed;
            {
                std::lock_guard lock(presentation.mutex);
                std::vector<RetiredSwapchain> kept;
                for (RetiredSwapchain& retired : presentation.retired)
                {
                    if (retired.frame > waited)
                    {
                        kept.push_back(std::move(retired));
                    }
                    else
                    {
                        freed.push_back(std::move(retired.swapchain));
                    }
                }
                presentation.retired = std::move(kept);
            }
            freeRetired(impl, freed);
        }

        //==============================================================================================================
        void releaseSwapchains(DeviceImpl& impl)
        {
            Presentation& presentation = impl.presentation;
            std::vector<std::unique_ptr<SwapchainImpl>> freed;
            {
                std::lock_guard lock(presentation.mutex);
                freed = std::move(presentation.swapchains);
                presentation.swapchains.clear();
                for (RetiredSwapchain& retired : presentation.retired)
                {
                    freed.push_back(std::move(retired.swapchain));
                }
                presentation.retired.clear();
            }
            // The queues are idle, and device destruction counts nothing.
            for (const std::unique_ptr<SwapchainImpl>& swapchain : freed)
            {
                freeSwapchain(*swapchain);
            }
            freed.clear();
            presentation.fence.reset();
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<Swapchain> Device::createSwapchain(const SwapchainDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The core checks the surface before anything reads it as a window; the extent comes from the swapchain afterwards.
        core::SwapchainCore state = core::prepareSwapchain(impl.core, desc, {});
        auto swapchain = std::make_unique<SwapchainImpl>();
        core::DeviceCounts::add(impl.core.counts().cpuAllocations);
        swapchain->core = std::move(state);
        // The window's uniqueness (VU-rojo-present-0006) is claimed before a destroyed swapchain on it is freed.
        impl.core.addSwapchain(swapchain->core);
        if (Result<void> made = createNative(impl, *swapchain, desc); !made)
        {
            impl.core.removeSwapchain(swapchain->core);
            freeSwapchain(*swapchain);
            return std::unexpected(std::move(made.error()));
        }
        SwapchainImpl* raw = swapchain.get();
        std::lock_guard lock(impl.presentation.mutex);
        impl.presentation.swapchains.push_back(std::move(swapchain));
        return SwapchainImpl::wrap(raw);
    }

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        SwapchainImpl& target = SwapchainImpl::of(swapchain);
        core::destroySwapchain(impl.core, target.core);
        // The swapchain and its back buffers outlive the call until `Device::beginFrame` has waited for its frame, as the native
        // objects of a deferred resource do, or until a new swapchain on its window frees it after its presents.
        retireSwapchain(impl, target);
    }

    //==================================================================================================================
    void Swapchain::resize(Extent2D extent)
    {
        SwapchainImpl& impl = SwapchainImpl::of(*this);
        core::resizeSwapchain(impl.core, extent);
        DeviceImpl& device = deviceOf(impl);
        // Every list that used the images was submitted before the last ended frame's fence signal on the graphics queue, and
        // every present of the swapchain came before it, so once the fence reaches that frame no work references the back
        // buffers. Before the first frame there is none.
        if (const uint64_t frame = device.core.frameNumber(); frame != 0)
        {
            const d3d12::FenceWait waited = d3d12::waitForFence(device.frames.fence(), frame, UINT64_MAX, "the frame fence");
            core::DeviceCounts::add(device.core.counts().nativeHostCalls, waited.hostCalls);
            // A removed device reports UINT64_MAX, which passes every frame; nothing more would run, so the call ends here.
            if (waited.completed == UINT64_MAX)
            {
                abortIfRemoved(device, "Swapchain::resize");
            }
        }
        d3d12::SwapchainNative& native = impl.native;
        // ResizeBuffers fails while any reference to a back buffer is held.
        {
            std::lock_guard lock(native.mutex);
            dropImages(native);
        }
        const auto count = static_cast<UINT>(native.images.size());
        d3d12::requireSucceeded(
            device,
            native.swapchain->ResizeBuffers(count, extent.width, extent.height, native.format, native.present.swapchainFlags),
            "IDXGISwapChain::ResizeBuffers in Swapchain::resize"
        );
        std::lock_guard lock(native.mutex);
        if (Result<void> fetched = fetchImages(device, native, impl.core); !fetched)
        {
            ROJORHI_LOG_ERROR("Swapchain::resize of \"{}\": {}", impl.core.label, fetched.error().message);
            std::abort();
        }
    }

    //==================================================================================================================
    Texture Swapchain::acquire()
    {
        SwapchainImpl& impl = SwapchainImpl::of(*this);
        core::checkAcquireImage(impl.core);
        DeviceImpl& device = deviceOf(impl);
        d3d12::SwapchainNative& native = impl.native;
        // The object is signalled while fewer than maxLatency presents are queued; a timeout changes no state.
        const DWORD waited = WaitForSingleObject(native.waitable, kAcquireDeadlineMs);
        core::DeviceCounts::add(device.core.counts().nativeHostCalls);
        if (waited == WAIT_TIMEOUT)
        {
            return {};
        }
        if (waited != WAIT_OBJECT_0)
        {
            ROJORHI_LOG_ERROR("the wait on the frame-latency object of swapchain \"{}\" returned {} with error {}", impl.core.label, waited, GetLastError());
            std::abort();
        }
        const UINT index = native.swapchain->GetCurrentBackBufferIndex();
        d3d12::TexturePayload* image = nullptr;
        {
            std::lock_guard lock(native.mutex);
            ROJORHI_INTERNAL_ASSERT(index < native.images.size(), "DXGI named a back buffer the swapchain does not have");
            image = native.images[index].get();
        }
        // The texture borrows the back buffer and its view: the record's swapchainImage flag keeps its retirement from them.
        return core::acquireImage(impl.core, image);
    }

    //==================================================================================================================
    void Queue::present(Swapchain swapchain)
    {
        QueueImpl& queue = QueueImpl::of(*this);
        SwapchainImpl& target = SwapchainImpl::of(swapchain);
        DeviceImpl& device = *queue.device;
        // The core checks the call and releases the image's texture, deferred, so the queued present still finds its buffer.
        core::presentImage(device.core, queue.type, target.core);
        d3d12::SwapchainNative& native = target.native;
        const HRESULT presented = native.swapchain->Present(native.present.interval, native.present.flags);
        core::DeviceCounts::add(device.core.counts().nativeSubmitCalls);
        native.presented = presented;
        if (!d3d12::presentSucceeded(presented))
        {
            if (d3d12::deviceRemoved(presented))
            {
                d3d12::reportRemoval(device, "Queue::present");
            }
            ROJORHI_LOG_ERROR("IDXGISwapChain::Present of swapchain \"{}\" returned HRESULT 0x{:08X}", target.core.label, d3d12::hresultBits(presented));
            std::abort();
        }
    }
} // namespace rojoRHI
