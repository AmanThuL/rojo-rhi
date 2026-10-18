//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capture.cpp
/// @brief Implements programmatic GPU capture through Metal's capture manager for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Capture.h"

#include <rojoRHI/RHI.h>

#include "Core/Captures.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace rojoRHI {
    namespace metal4 {
        //==============================================================================================================
        Result<void> startCapture(std::string_view path)
        {
            const AutoreleasePool pool;
            MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
            if (manager == nullptr || !manager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument))
            {
                return std::unexpected(
                    Error{ ErrorCategory::Unsupported, "this process cannot write a .gputrace document; relaunch it with MTL_CAPTURE_ENABLED=1" }
                );
            }
            if (manager->isCapturing())
            {
                return std::unexpected(Error{ ErrorCategory::Unsupported, "another capture is in progress" });
            }

            // The capture's URL outlives this call's working directory, so it names the absolute path.
            std::error_code pathError;
            const std::filesystem::path absolute = std::filesystem::absolute(path, pathError);
            if (pathError)
            {
                return std::unexpected(
                    Error{ ErrorCategory::Unsupported, std::format("cannot resolve the capture path \"{}\": {}", path, pathError.message()) }
                );
            }

            MTL::Device* device = MTL::CreateSystemDefaultDevice();
            if (device == nullptr)
            {
                return std::unexpected(Error{ ErrorCategory::Unsupported, "there is no Metal device to capture" });
            }
            MTL::CaptureDescriptor* descriptor = MTL::CaptureDescriptor::alloc()->init();
            descriptor->setCaptureObject(device);
            descriptor->setDestination(MTL::CaptureDestinationGPUTraceDocument);
            descriptor->setOutputURL(NS::URL::fileURLWithPath(makeString(absolute.string())));

            NS::Error* error = nullptr;
            const bool started = manager->startCapture(descriptor, &error);
            const std::string reason = started ? std::string() : describe(error);
            descriptor->release();
            device->release();
            if (!started)
            {
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("Metal refused the capture to \"{}\": {}", absolute.string(), reason) }
                );
            }
            return {};
        }
    } // namespace metal4

    //==================================================================================================================
    Result<void> Device::beginCapture(std::string_view path)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        Result<void> started = core::beginCapture(impl.core, path);
        if (started.has_value())
        {
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        }
        return started;
    }

    //==================================================================================================================
    void Device::endCapture()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::endCapture(impl.core);
        const metal4::AutoreleasePool pool;
        MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
        // The manager is process-global: in Release, where VU-rojo-diag-0010 is unchecked, an unmatched call may stop another
        // device's capture, which the caller is responsible for avoiding.
        if (manager != nullptr && manager->isCapturing())
        {
            manager->stopCapture();
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        }
    }
} // namespace rojoRHI
