//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capture.cpp
/// @brief Implements Xcode GPU capture lifecycle controls for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/Metal4/Metal4Capture.h>

#include "Base/Log.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include <rojoRHI/CaptureSchema.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace rojoRHI::metal4 {
namespace {

// MTLCaptureManager is process-global and supports one active capture.
std::string g_capturePath;
std::string g_captureFailure;

} // namespace

//======================================================================================================================
bool captureAvailable() {
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    auto* manager = MTL::CaptureManager::sharedCaptureManager();
    return manager != nullptr &&
           manager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument);
}

//======================================================================================================================
std::string_view captureFailureReason() {
    return g_captureFailure;
}

//======================================================================================================================
bool beginCapture(Device& device, std::string_view outPath) {
    g_captureFailure.clear();
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    ROJORHI_ASSERT(manager != nullptr, "beginCapture: no shared capture manager");

    // Metal reads MTL_CAPTURE_ENABLED at launch; a refused capture requires process restart.
    if (!manager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument)) {
        g_captureFailure =
            "Relaunch with MTL_CAPTURE_ENABLED=1 xmake run App to enable GPU capture.";
        ROJORHI_LOG_WARN("GPU capture unavailable: this process cannot write a .gputrace document. "
                         "Relaunch with capture enabled in the environment, e.g. "
                         "`MTL_CAPTURE_ENABLED=1 xmake run App`");
        return false;
    }
    if (manager->isCapturing()) {
        g_captureFailure = "A GPU capture is already in progress. Wait for it to finish.";
        ROJORHI_LOG_WARN("GPU capture already in progress; ignoring the request");
        return false;
    }

    // Validate before remove_all: an empty path resolves to the working directory, while the
    // suffix independently restricts deletion to capture bundles.
    if (outPath.empty()) {
        g_captureFailure = "Output path must not be empty.";
        ROJORHI_LOG_ERROR("GPU capture: outPath must not be empty");
        return false;
    }
    constexpr std::string_view kRequiredSuffix = ".gputrace";
    if (!outPath.ends_with(kRequiredSuffix)) {
        g_captureFailure = "Output path must end in .gputrace. Set LMX_CAPTURE_PATH and relaunch.";
        ROJORHI_LOG_ERROR("GPU capture: outPath '{}' must end in '{}'", outPath, kRequiredSuffix);
        return false;
    }

    // Capture URLs outlive this call's working-directory context.
    std::error_code pathError;
    std::filesystem::path path = std::filesystem::absolute(outPath, pathError);
    if (pathError) {
        g_captureFailure = "Cannot resolve output path: " + pathError.message();
        ROJORHI_LOG_ERROR("GPU capture: cannot resolve output path '{}': {}", outPath,
                          pathError.message());
        return false;
    }

    // Metal refuses to overwrite an existing capture bundle.
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
    if (removeError) {
        g_captureFailure = "Cannot replace capture document: " + removeError.message();
        ROJORHI_LOG_ERROR("GPU capture: cannot remove the existing document at '{}': {}",
                          path.string(), removeError.message());
        return false;
    }

    auto url =
        NS::TransferPtr(NS::URL::alloc()->initFileURLWithPath(makeString(path.string()).get()));

    auto desc = NS::TransferPtr(MTL::CaptureDescriptor::alloc()->init());
    // Device capture is Metal's widest available scope.
    desc->setCaptureObject(static_cast<Metal4Device&>(device).handle());
    desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
    desc->setOutputURL(url.get());

    NS::Error* error = nullptr;
    if (!manager->startCapture(desc.get(), &error)) {
        const NS::String* reason = error != nullptr ? error->localizedDescription() : nullptr;
        const char* utf8 = reason != nullptr ? reason->utf8String() : nullptr;
        g_captureFailure = utf8 != nullptr ? utf8 : "Metal did not provide a failure reason.";
        ROJORHI_LOG_ERROR("GPU capture failed to start: {}",
                          utf8 != nullptr ? utf8 : "no additional detail");
        return false;
    }

    g_capturePath = path.string();
    // Record sidecar data only while Metal's capture window is active.
    debug::CaptureSchema::instance().beginFrameRecords();
    ROJORHI_LOG_INFO("GPU capture started -> {}", g_capturePath);
    return true;
}

//======================================================================================================================
void endCapture() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    ROJORHI_ASSERT(manager != nullptr, "endCapture: no shared capture manager");
    if (!manager->isCapturing()) {
        return;
    }

    manager->stopCapture();

    // External captures have no project-owned bundle path for a sidecar.
    debug::CaptureSchema& schema = debug::CaptureSchema::instance();
    if (!g_capturePath.empty()) {
        // Sidecar failure does not invalidate the GPU capture.
        schema.writeJson(g_capturePath + ".schema.json");
    }
    schema.endFrameRecords();

    ROJORHI_LOG_INFO("GPU capture written: {} (open it with `open {}`)", g_capturePath,
                     g_capturePath);
    g_capturePath.clear();
}

} // namespace rojoRHI::metal4
