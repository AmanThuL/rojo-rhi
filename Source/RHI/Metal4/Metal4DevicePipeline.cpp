#include "RHI/Metal4/Metal4Device.h"

#include "Core/Log.h"
#include "RHI/Metal4/Metal4DevicePrivate.h"
#include "RHI/Metal4/Metal4Resources.h"
#include "RHI/Validate.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace lmx::rhi::metal4 {
namespace {

using device_detail::describe;
using device_detail::fail;
using device_detail::labelOrFallback;
using device_detail::toStdString;

constexpr MTL::LanguageVersion kShaderLanguageVersion = MTL::LanguageVersion4_0;

//======================================================================================================================
MTL::TriangleFillMode toMTL(FillMode mode) {
    return mode == FillMode::Wireframe ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
}

//======================================================================================================================
MTL::CullMode toMTL(CullMode mode) {
    return mode == CullMode::Back ? MTL::CullModeBack : MTL::CullModeNone;
}

//======================================================================================================================
MTL::CompareFunction toMTL(DepthCompare compare) {
    switch (compare) {
    case DepthCompare::Less:
        return MTL::CompareFunctionLess;
    case DepthCompare::LessEqual:
        return MTL::CompareFunctionLessEqual;
    case DepthCompare::Greater:
        return MTL::CompareFunctionGreater;
    case DepthCompare::GreaterEqual:
        return MTL::CompareFunctionGreaterEqual;
    }
    return MTL::CompareFunctionLess;
}

//======================================================================================================================
std::vector<std::string> libraryFunctionNames(MTL::Library* library) {
    std::vector<std::string> names;
    NS::Array* array = library->functionNames();
    if (array == nullptr) {
        return names;
    }
    names.reserve(array->count());
    for (NS::UInteger i = 0; i < array->count(); ++i) {
        names.push_back(toStdString(array->object<NS::String>(i)));
    }
    return names;
}

//======================================================================================================================
const char* describe(MTL::FunctionType type) {
    switch (type) {
    case MTL::FunctionTypeVertex:
        return "vertex";
    case MTL::FunctionTypeFragment:
        return "fragment";
    case MTL::FunctionTypeKernel:
        return "kernel";
    case MTL::FunctionTypeVisible:
        return "visible";
    case MTL::FunctionTypeIntersection:
        return "intersection";
    case MTL::FunctionTypeMesh:
        return "mesh";
    case MTL::FunctionTypeObject:
        return "object";
    }
    return "unknown";
}

//======================================================================================================================
std::string join(const std::vector<std::string>& items) {
    if (items.empty()) {
        return "<none>";
    }
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += item;
    }
    return joined;
}

//======================================================================================================================
std::optional<std::string> readTextFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    if (stream.bad()) {
        return std::nullopt;
    }
    return contents;
}

} // namespace

//======================================================================================================================
Result<std::unique_ptr<ShaderLibrary>> Metal4Device::loadShaderLibrary(std::string_view pathNoExt) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Copy the view before passing it to path APIs that require owned, terminated storage.
    const std::string base(pathNoExt);
    const std::filesystem::path libraryPath(base + ".metallib");
    const std::filesystem::path sourcePath(base + ".metal");

    NS::Error* error = nullptr;

    // Prefer an offline metallib and fall back to readable MSL. Non-throwing stat failures are
    // treated as absence; the final error reports both candidate paths.
    std::error_code statError;
    if (std::filesystem::is_regular_file(libraryPath, statError)) {
        NS::SharedPtr<NS::URL> url = NS::TransferPtr(
            NS::URL::alloc()->initFileURLWithPath(makeString(libraryPath.string()).get()));
        NS::SharedPtr<MTL::Library> library =
            NS::TransferPtr(m_device->newLibrary(url.get(), &error));
        if (!library) {
            return fail(ErrorCode::ShaderLoadFailed, "failed to load precompiled library '" +
                                                         libraryPath.string() +
                                                         "': " + describe(error));
        }
        library->setLabel(makeString(base).get());
        LMX_LOG_INFO("shader library '{}': loaded precompiled metallib", base);
        return std::make_unique<Metal4ShaderLibrary>(std::move(library));
    }

    if (std::filesystem::is_regular_file(sourcePath, statError)) {
        const std::optional<std::string> source = readTextFile(sourcePath);
        if (!source) {
            return fail(ErrorCode::ShaderLoadFailed,
                        "failed to read shader source '" + sourcePath.string() + "'");
        }
        auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
        options->setLanguageVersion(kShaderLanguageVersion);

        error = nullptr;
        NS::SharedPtr<MTL::Library> library =
            NS::TransferPtr(m_device->newLibrary(makeString(*source).get(), options.get(), &error));
        if (!library) {
            return fail(ErrorCode::ShaderLoadFailed, "failed to compile shader source '" +
                                                         sourcePath.string() +
                                                         "': " + describe(error));
        }
        library->setLabel(makeString(base).get());
        LMX_LOG_INFO("shader library '{}': compiled MSL source at runtime", base);
        return std::make_unique<Metal4ShaderLibrary>(std::move(library));
    }

    return fail(ErrorCode::ShaderLoadFailed, "shader library not found: tried '" +
                                                 libraryPath.string() + "' and '" +
                                                 sourcePath.string() + "'");
}

//======================================================================================================================
Result<std::unique_ptr<GraphicsPipeline>>
Metal4Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    auto* library = static_cast<Metal4ShaderLibrary*>(desc.library);

    // Metal aborts on nil or wrong-stage entry points, so validate both before pipeline creation.
    const auto rejectEntry = [&](std::string_view entry,
                                 MTL::FunctionType expected) -> std::optional<std::string> {
        NS::SharedPtr<MTL::Function> function =
            NS::TransferPtr(library->handle()->newFunction(makeString(entry).get()));
        if (!function) {
            return "shader entry point '" + std::string(entry) +
                   "' not found in library; available entry points: " +
                   join(libraryFunctionNames(library->handle()));
        }
        if (function->functionType() != expected) {
            return "shader entry point '" + std::string(entry) + "' is a " +
                   describe(function->functionType()) + " function, but a " + describe(expected) +
                   " function is required here";
        }
        return std::nullopt;
    };

    if (auto problem = rejectEntry(desc.vertexEntry, MTL::FunctionTypeVertex)) {
        return fail(ErrorCode::PipelineCreationFailed, std::move(*problem));
    }
    if (auto problem = rejectEntry(desc.fragmentEntry, MTL::FunctionTypeFragment)) {
        return fail(ErrorCode::PipelineCreationFailed, std::move(*problem));
    }

    auto vertexFunction = NS::TransferPtr(MTL4::LibraryFunctionDescriptor::alloc()->init());
    vertexFunction->setLibrary(library->handle());
    vertexFunction->setName(makeString(desc.vertexEntry).get());

    auto fragmentFunction = NS::TransferPtr(MTL4::LibraryFunctionDescriptor::alloc()->init());
    fragmentFunction->setLibrary(library->handle());
    fragmentFunction->setName(makeString(desc.fragmentEntry).get());

    auto pipelineDesc = NS::TransferPtr(MTL4::RenderPipelineDescriptor::alloc()->init());
    pipelineDesc->setVertexFunctionDescriptor(vertexFunction.get());
    pipelineDesc->setFragmentFunctionDescriptor(fragmentFunction.get());
    // PixelFormatInvalid is Metal's pipeline representation for no color attachment.
    if (desc.colorFormat != Format::Unknown) {
        pipelineDesc->colorAttachments()->object(0)->setPixelFormat(toMTL(desc.colorFormat));
    }
    pipelineDesc->setRasterSampleCount(1);
    // Pipeline-state labels are inherited from the descriptor; the state has no setter.
    pipelineDesc->setLabel(labelOrFallback(desc.label, "lmx.pipeline.unnamed").get());

    NS::Error* error = nullptr;
    NS::SharedPtr<MTL::RenderPipelineState> state =
        NS::TransferPtr(m_compiler->newRenderPipelineState(
            pipelineDesc.get(), /*compilerTaskOptions=*/nullptr, &error));
    if (!state) {
        return fail(ErrorCode::PipelineCreationFailed,
                    "failed to create render pipeline (vertex '" + std::string(desc.vertexEntry) +
                        "', fragment '" + std::string(desc.fragmentEntry) +
                        "'): " + describe(error));
    }

    // Metal depth state is encoder state. Null preserves compare-always/no-write; the attachment
    // supplies its own depth format because MTL4::RenderPipelineDescriptor has no such field.
    NS::SharedPtr<MTL::DepthStencilState> depthState;
    if (desc.depthTestEnable || desc.depthWriteEnable) {
        auto dsDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
        dsDesc->setDepthCompareFunction(desc.depthTestEnable ? toMTL(desc.depthCompare)
                                                             : MTL::CompareFunctionAlways);
        dsDesc->setDepthWriteEnabled(desc.depthWriteEnable);
        // Distinguish the separate depth-state object in captures.
        const std::string_view pipelineLabel =
            desc.label.empty() ? std::string_view("lmx.pipeline.unnamed") : desc.label;
        dsDesc->setLabel(makeString(std::string(pipelineLabel) + ".depth").get());
        depthState = NS::TransferPtr(m_device->newDepthStencilState(dsDesc.get()));
        if (!depthState) {
            return fail(ErrorCode::PipelineCreationFailed, "failed to create depth-stencil state");
        }
    }

    // Convert encoder-owned raster state once and replay it at bind time.
    const Metal4RasterState rasterState{.fillMode = toMTL(desc.fillMode),
                                        .cullMode = toMTL(desc.cullMode),
                                        .frontFacingWinding = kFrontFacingWinding,
                                        .depthBias = desc.depthBias.constant,
                                        .slopeScale = desc.depthBias.slopeScale,
                                        .biasClamp = desc.depthBias.clamp};

    return std::make_unique<Metal4Pipeline>(std::move(state), std::move(depthState), rasterState);
}

} // namespace lmx::rhi::metal4
