#include "RHI/Metal4/Metal4Capture.h"

#include "Core/Log.h"
#include "RHI/CaptureSchema.h"
#include "RHI/Metal4/Metal4Common.h"
#include "RHI/Metal4/Metal4Device.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace lmx::rhi::metal4 {
namespace {

// MTLCaptureManager is process-global and supports one active capture.
std::string g_capturePath;

} // namespace

//======================================================================================================================
bool beginCapture(Device& device, std::string_view outPath) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    LMX_ASSERT(manager != nullptr, "beginCapture: no shared capture manager");

    // Metal reads MTL_CAPTURE_ENABLED at launch; a refused capture requires process restart.
    if (!manager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument)) {
        LMX_LOG_WARN("GPU capture unavailable: this process cannot write a .gputrace document. "
                     "Relaunch with capture enabled in the environment, e.g. "
                     "`MTL_CAPTURE_ENABLED=1 xmake run App`");
        return false;
    }
    if (manager->isCapturing()) {
        LMX_LOG_WARN("GPU capture already in progress; ignoring the request");
        return false;
    }

    // Validate before remove_all: an empty path resolves to the working directory, while the
    // suffix independently restricts deletion to capture bundles.
    if (outPath.empty()) {
        LMX_LOG_ERROR("GPU capture: outPath must not be empty");
        return false;
    }
    constexpr std::string_view kRequiredSuffix = ".gputrace";
    if (!outPath.ends_with(kRequiredSuffix)) {
        LMX_LOG_ERROR("GPU capture: outPath '{}' must end in '{}'", outPath, kRequiredSuffix);
        return false;
    }

    // Capture URLs outlive this call's working-directory context.
    std::error_code pathError;
    std::filesystem::path path = std::filesystem::absolute(outPath, pathError);
    if (pathError) {
        LMX_LOG_ERROR("GPU capture: cannot resolve output path '{}': {}", outPath,
                      pathError.message());
        return false;
    }

    // Metal refuses to overwrite an existing capture bundle.
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
    if (removeError) {
        LMX_LOG_ERROR("GPU capture: cannot remove the existing document at '{}': {}", path.string(),
                      removeError.message());
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
        LMX_LOG_ERROR("GPU capture failed to start: {}",
                      utf8 != nullptr ? utf8 : "no additional detail");
        return false;
    }

    g_capturePath = path.string();
    // Record sidecar data only while Metal's capture window is active.
    debug::CaptureSchema::instance().beginFrameRecords();
    LMX_LOG_INFO("GPU capture started -> {}", g_capturePath);
    return true;
}

//======================================================================================================================
void endCapture() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    LMX_ASSERT(manager != nullptr, "endCapture: no shared capture manager");
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

    LMX_LOG_INFO("GPU capture written: {} (open it with `open {}`)", g_capturePath, g_capturePath);
    g_capturePath.clear();
}

} // namespace lmx::rhi::metal4
