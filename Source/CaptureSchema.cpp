//----------------------------------------------------------------------------------------------------------------------
/// @file CaptureSchema.cpp
/// @brief Serializes deterministic resource and uniform metadata beside GPU captures.
//----------------------------------------------------------------------------------------------------------------------
#include "RHI/CaptureSchema.h"

#include "Core/Log.h"

#include <cmath>
#include <format>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>

namespace lmx::rhi::debug {
namespace {

//======================================================================================================================
// Serialize enumerator names verbatim for the capture tools. Omitting default lets -Wswitch catch
// newly added formats.
std::string_view formatName(Format format) {
    switch (format) {
    case Format::Unknown:
        return "Unknown";
    case Format::BGRA8Unorm:
        return "BGRA8Unorm";
    case Format::RGBA8Unorm:
        return "RGBA8Unorm";
    case Format::RGBA8Unorm_sRGB:
        return "RGBA8Unorm_sRGB";
    case Format::RGBA16Float:
        return "RGBA16Float";
    case Format::RG16Float:
        return "RG16Float";
    case Format::R16Float:
        return "R16Float";
    case Format::R8Unorm:
        return "R8Unorm";
    case Format::BC1Unorm:
        return "BC1Unorm";
    case Format::BC1Unorm_sRGB:
        return "BC1Unorm_sRGB";
    case Format::D32Float:
        return "D32Float";
    }
    return "Unknown";
}

//======================================================================================================================
// Escape RFC 8259 control bytes while preserving existing UTF-8 label bytes.
void appendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"') {
            out += "\\\"";
        } else if (byte == '\\') {
            out += "\\\\";
        } else if (byte < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
        } else {
            out += character;
        }
    }
    out += '"';
}

//======================================================================================================================
// JSON has no non-finite number syntax; emit null so a degenerate value remains parseable.
void appendFloat(std::string& out, float value) {
    if (!std::isfinite(value)) {
        LMX_LOG_WARN("capture schema: non-finite float in the context, writing null");
        out += "null";
        return;
    }
    out += std::format("{}", value);
}

//======================================================================================================================
void appendFloatArray(std::string& out, std::span<const float> values) {
    out += '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        appendFloat(out, values[i]);
    }
    out += ']';
}

} // namespace

//======================================================================================================================
CaptureSchema& CaptureSchema::instance() {
    static CaptureSchema schema;
    return schema;
}

//======================================================================================================================
void CaptureSchema::upsert(const void* handle, Resource resource) {
    for (auto& entry : m_resources) {
        if (entry.first == handle) {
            entry.second = std::move(resource);
            return;
        }
    }
    m_resources.emplace_back(handle, std::move(resource));
}

//======================================================================================================================
void CaptureSchema::registerTexture(const void* handle, std::string_view label,
                                    const TextureDesc& desc) {
    Resource resource;
    resource.label = std::string(label);
    resource.kind = desc.kind == TextureKind::Cube ? ResourceKind::Cube : ResourceKind::Texture2D;
    resource.format = desc.format;
    resource.width = desc.width;
    resource.height = desc.height;
    resource.mipLevels = desc.mipLevels;
    upsert(handle, std::move(resource));
}

//======================================================================================================================
void CaptureSchema::registerBuffer(const void* handle, std::string_view label, uint64_t sizeBytes) {
    Resource resource;
    resource.label = std::string(label);
    resource.kind = ResourceKind::Buffer;
    resource.sizeBytes = sizeBytes;
    upsert(handle, std::move(resource));
}

//======================================================================================================================
// Capture bookkeeping is observational; unregistering an unknown handle is a harmless no-op.
void CaptureSchema::unregisterResource(const void* handle) {
    for (auto it = m_resources.begin(); it != m_resources.end(); ++it) {
        if (it->first == handle) {
            m_resources.erase(it);
            return;
        }
    }
}

//======================================================================================================================
void CaptureSchema::registerUniformStruct(SchemaUniformStruct layout) {
    for (auto& existing : m_uniformStructs) {
        if (existing.name == layout.name) {
            existing = std::move(layout);
            return;
        }
    }
    m_uniformStructs.push_back(std::move(layout));
}

//======================================================================================================================
// Clear frame-local uploads and context while retaining device and shader metadata.
void CaptureSchema::beginFrameRecords() {
    m_frameDataUploads.clear();
    m_context = SchemaContext{};
    m_recording = true;
}

//======================================================================================================================
// Stop recording without clearing the records that writeJson still needs.
void CaptureSchema::endFrameRecords() {
    m_recording = false;
}

//======================================================================================================================
bool CaptureSchema::recordingUploads() const {
    return m_recording;
}

//======================================================================================================================
void CaptureSchema::recordFrameDataUpload(SchemaFrameDataUpload upload) {
    if (!m_recording) {
        return;
    }
    m_frameDataUploads.push_back(std::move(upload));
}

//======================================================================================================================
void CaptureSchema::setContext(SchemaContext context) {
    m_context = std::move(context);
}

//======================================================================================================================
// The fixed, flat schema does not warrant a JSON dependency; keep one record per readable line.
std::string CaptureSchema::renderJson() const {
    const auto kindName = [](ResourceKind kind) -> std::string_view {
        switch (kind) {
        case ResourceKind::Texture2D:
            return "texture2d";
        case ResourceKind::Cube:
            return "cube";
        case ResourceKind::Buffer:
            return "buffer";
        }
        return "buffer";
    };

    std::string out = "{\n  \"version\": 1,\n";

    out += "  \"context\": {\n    \"sceneName\": ";
    appendJsonString(out, m_context.sceneName);
    out += std::format(",\n    \"frameIndex\": {},\n    \"cameraPos\": ", m_context.frameIndex);
    appendFloatArray(out, m_context.cameraPos);
    out += ",\n    \"boundingSphere\": ";
    appendFloatArray(out, m_context.boundingSphere);
    out += ",\n    \"light0Direction\": ";
    appendFloatArray(out, m_context.light0Direction);
    out += ",\n    \"light0Strength\": ";
    appendFloatArray(out, m_context.light0Strength);
    out += ",\n    \"shadowFilter\": ";
    appendJsonString(out, m_context.shadowFilter);
    out += "\n  },\n";

    out += "  \"resources\": [";
    for (size_t i = 0; i < m_resources.size(); ++i) {
        const Resource& resource = m_resources[i].second;
        out += i == 0 ? "\n    " : ",\n    ";
        out += "{\"label\": ";
        appendJsonString(out, resource.label);
        out += std::format(", \"kind\": \"{}\"", kindName(resource.kind));
        if (resource.kind == ResourceKind::Buffer) {
            out += std::format(", \"sizeBytes\": {}", resource.sizeBytes);
        } else {
            out += std::format(", \"format\": \"{}\", \"width\": {}, \"height\": {}, "
                               "\"mipLevels\": {}",
                               formatName(resource.format), resource.width, resource.height,
                               resource.mipLevels);
        }
        out += '}';
    }
    out += m_resources.empty() ? "],\n" : "\n  ],\n";

    out += "  \"uniformStructs\": [";
    for (size_t i = 0; i < m_uniformStructs.size(); ++i) {
        const SchemaUniformStruct& layout = m_uniformStructs[i];
        out += i == 0 ? "\n    " : ",\n    ";
        out += "{\"name\": ";
        appendJsonString(out, layout.name);
        out += std::format(", \"slot\": {}, \"sizeBytes\": {}, \"fields\": [", layout.slot,
                           layout.sizeBytes);
        for (size_t field = 0; field < layout.fields.size(); ++field) {
            out += field == 0 ? "\n      " : ",\n      ";
            out += "{\"name\": ";
            appendJsonString(out, layout.fields[field].name);
            out +=
                std::format(", \"offsetBytes\": {}, \"type\": ", layout.fields[field].offsetBytes);
            appendJsonString(out, layout.fields[field].type);
            out += '}';
        }
        out += layout.fields.empty() ? "]}" : "\n    ]}";
    }
    out += m_uniformStructs.empty() ? "],\n" : "\n  ],\n";

    out += "  \"frameDataUploads\": [";
    for (size_t i = 0; i < m_frameDataUploads.size(); ++i) {
        const SchemaFrameDataUpload& upload = m_frameDataUploads[i];
        out += i == 0 ? "\n    " : ",\n    ";
        out += "{\"pageLabel\": ";
        appendJsonString(out, upload.pageLabel);
        out += std::format(", \"slot\": {}, \"pageOffset\": {}, \"sizeBytes\": {}, "
                           "\"alignmentBytes\": {}, \"gpuAddress\": {}}}",
                           upload.slot, upload.pageOffset, upload.sizeBytes, upload.alignmentBytes,
                           upload.gpuAddress);
    }
    out += m_frameDataUploads.empty() ? "]\n}\n" : "\n  ]\n}\n";

    return out;
}

//======================================================================================================================
bool CaptureSchema::writeJson(const std::filesystem::path& path) const {
    const std::string text = renderJson();

    // Write beside the target and rename so readers observe either complete document.
    std::filesystem::path temp = path;
    temp += ".tmp";

    std::error_code ignored;
    std::filesystem::remove(temp, ignored); // a leftover from a run that died mid-write

    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
        LMX_LOG_ERROR("capture schema: cannot open '{}' for writing", temp.string());
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file) {
        LMX_LOG_ERROR("capture schema: failed while writing '{}'", temp.string());
        std::filesystem::remove(temp, ignored);
        return false;
    }

    std::error_code renameError;
    std::filesystem::rename(temp, path, renameError);
    if (renameError) {
        LMX_LOG_ERROR("capture schema: cannot move '{}' onto '{}': {}", temp.string(),
                      path.string(), renameError.message());
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}

//======================================================================================================================
void CaptureSchema::resetForTest() {
    m_resources.clear();
    m_uniformStructs.clear();
    m_frameDataUploads.clear();
    m_context = SchemaContext{};
    m_recording = false;
}

} // namespace lmx::rhi::debug
