// Source/RHI/CaptureSchema.h -- developer tooling, not part of the RHI surface (the
// Metal4Capture precedent): a metal-free, glm-free collector the backend, renderer and app all
// feed, flushed to "<bundle>.schema.json" when a capture closes. Single-threaded by
// construction, like the backend it observes.
#pragma once
#include "RHI/RHI.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lmx::rhi::debug {

struct SchemaUniformField {
    std::string name;
    uint32_t offsetBytes = 0;
    std::string type; // "float" | "float3" | "float4" | "float4x4" | "int" | "uint"
};
struct SchemaUniformStruct {
    std::string name;
    uint32_t slot = 0;
    uint32_t sizeBytes = 0;
    std::vector<SchemaUniformField> fields;
};
struct SchemaUniformUpload {
    std::string ringLabel;
    uint32_t slot = 0;
    uint64_t ringOffset = 0;
    uint64_t sizeBytes = 0;
};
struct SchemaContext {
    std::string sceneName;
    uint64_t frameIndex = 0;
    std::array<float, 3> cameraPos{};
    std::array<float, 4> boundingSphere{}; // xyz center, w radius
    std::array<float, 3> light0Direction{};
    std::array<float, 3> light0Strength{};
    std::string shadowFilter; // "PCF" | "PCSS"
};

class CaptureSchema {
public:
    static CaptureSchema& instance();

    // Registry. `handle` is an identity key only (never dereferenced); labels are the resolved
    // ones (post labelOrFallback). Idempotent per handle: re-registering replaces.
    void registerTexture(const void* handle, std::string_view label, const TextureDesc& desc);
    void registerBuffer(const void* handle, std::string_view label, uint64_t sizeBytes);
    void unregisterResource(const void* handle);

    // Layouts: replaces any previous struct with the same name (idempotent for re-creates).
    void registerUniformStruct(SchemaUniformStruct layout);

    // Frame-record window, driven by begin/endCapture.
    void beginFrameRecords(); // clears uploads + context, starts recording
    void endFrameRecords();   // stops recording
    bool recordingUploads() const;
    void recordUniformUpload(SchemaUniformUpload upload); // no-op unless recording

    void setContext(SchemaContext context);

    // Writes the sidecar; returns false (after logging) on I/O failure -- callers never abort.
    bool writeJson(const std::filesystem::path& path) const;

    void resetForTest(); // tests only: full clear

private:
    CaptureSchema() = default;

    // What a registered resource contributes to the sidecar's flat `resources` array. Textures
    // and buffers share one record type -- `kind` decides which fields the JSON carries -- because
    // consumers read the array positionally and a second array would buy nothing.
    enum class ResourceKind { Texture2D, Cube, Buffer };
    struct Resource {
        std::string label;
        ResourceKind kind = ResourceKind::Buffer;
        Format format = Format::Unknown;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
        uint64_t sizeBytes = 0;
    };

    // Registers `resource` under `handle`, replacing whatever that handle held before.
    void upsert(const void* handle, Resource resource);
    std::string renderJson() const;

    // Insertion-ordered association list keyed by the handle pointer, not a map: N is the few
    // dozen resources a device owns, every lookup happens once per create/destroy, and preserving
    // creation order is what makes two runs' sidecars diffable.
    std::vector<std::pair<const void*, Resource>> m_resources;
    std::vector<SchemaUniformStruct> m_uniformStructs;
    std::vector<SchemaUniformUpload> m_uniformUploads;
    SchemaContext m_context;
    bool m_recording = false;
};

} // namespace lmx::rhi::debug
