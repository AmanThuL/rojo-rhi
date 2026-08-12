//----------------------------------------------------------------------------------------------------------------------
/// @file CaptureSchema.h
/// @brief Declares the schema sidecar collected for Metal GPU captures.
//----------------------------------------------------------------------------------------------------------------------
/// Developer tooling, not part of the core RHI surface (the
/// Metal4Capture precedent): a metal-free, glm-free collector the backend, renderer and app all
/// feed, flushed to "<bundle>.schema.json" when a capture closes. Single-threaded by
/// construction, like the backend it observes.
#pragma once
#include "RHI/Format.h"
#include "RHI/Texture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lmx::rhi::debug {

/// Describes one named field in a captured uniform layout.
struct SchemaUniformField {
    std::string name;         ///< Field name as declared in the shader layout.
    uint32_t offsetBytes = 0; ///< Byte offset from the beginning of the uniform structure.
    std::string type;         ///< Shader scalar, vector, or matrix type name.
};
/// Describes a captured uniform structure and its argument-table slot.
struct SchemaUniformStruct {
    std::string name;                       ///< Stable uniform structure name.
    uint32_t slot = 0;                      ///< Argument-table buffer slot.
    uint32_t sizeBytes = 0;                 ///< Total structure size in bytes.
    std::vector<SchemaUniformField> fields; ///< Ordered field layout.
};
/// Records one block published into a frame slot's per-frame data arena.
///
/// The page label plus the offset name exactly one range of one labeled allocation, which is what
/// makes an address seen in a capture traceable back to the call that produced it.
struct SchemaFrameDataUpload {
    std::string pageLabel;       ///< Diagnostic label of the arena page holding the block.
    uint32_t slot = 0;           ///< Argument-table buffer slot receiving the block's address.
    uint64_t pageOffset = 0;     ///< Byte offset of the block from the page's base.
    uint64_t sizeBytes = 0;      ///< Copied byte count, excluding alignment padding.
    uint64_t alignmentBytes = 0; ///< Alignment the block was placed at.
    uint64_t gpuAddress = 0;     ///< GPU address handed back to the caller.
};
/// Captures scene, camera, light, and shadow-filter context for one frame.
struct SchemaContext {
    std::string sceneName;                  ///< Stable scene identifier.
    uint64_t frameIndex = 0;                ///< Captured application frame index.
    std::array<float, 3> cameraPos{};       ///< World-space camera position.
    std::array<float, 4> boundingSphere{};  ///< World-space xyz center and radius in w.
    std::array<float, 3> light0Direction{}; ///< Primary directional-light direction.
    std::array<float, 3> light0Strength{};  ///< Primary directional-light linear strength.
    std::string shadowFilter;               ///< Active shadow filter name, PCF or PCSS.
};

/// Collects deterministic resource and uniform metadata beside a GPU capture.
class CaptureSchema {
public:
    /// Returns the process-wide capture schema collector.
    static CaptureSchema& instance();

    /// Registry. `handle` is an identity key only (never dereferenced); labels are the resolved
    /// ones (post labelOrFallback). Idempotent per handle: re-registering replaces.
    /// Registers or replaces texture metadata keyed by an opaque identity handle.
    void registerTexture(const void* handle, std::string_view label, const TextureDesc& desc);
    /// Registers or replaces buffer metadata keyed by an opaque identity handle.
    void registerBuffer(const void* handle, std::string_view label, uint64_t sizeBytes);
    /// Removes metadata associated with an opaque resource handle.
    void unregisterResource(const void* handle);

    /// Layouts: replaces any previous struct with the same name (idempotent for re-creates).
    /// Registers or replaces a named uniform structure layout.
    void registerUniformStruct(SchemaUniformStruct layout);

    /// Frame-record window, driven by begin/endCapture.
    /// Clears frame data and begins accepting frame-data upload records.
    void beginFrameRecords();
    /// Stops accepting frame-data upload records.
    void endFrameRecords();
    /// Returns whether frame-data uploads are currently being recorded.
    bool recordingUploads() const;
    /// Records a frame-data block when the frame-record window is active.
    void recordFrameDataUpload(SchemaFrameDataUpload upload);

    /// Replaces the contextual metadata for the current capture frame.
    void setContext(SchemaContext context);

    /// Writes the sidecar; returns false (after logging) on I/O failure -- callers never abort.
    /// Writes the schema sidecar, returning false after logging an I/O failure.
    bool writeJson(const std::filesystem::path& path) const;

    /// Clears all collected state for deterministic tests.
    void resetForTest();

private:
    CaptureSchema() = default;

    /// What a registered resource contributes to the sidecar's flat `resources` array. Textures
    /// and buffers share one record type -- `kind` decides which fields the JSON carries -- because
    /// consumers read the array positionally and a second array would buy nothing.
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

    /// Registers `resource` under `handle`, replacing whatever that handle held before.
    void upsert(const void* handle, Resource resource);
    std::string renderJson() const;

    /// Insertion-ordered association list keyed by the handle pointer, not a map: N is the few
    /// dozen resources a device owns, every lookup happens once per create/destroy, and preserving
    /// creation order is what makes two runs' sidecars diffable.
    std::vector<std::pair<const void*, Resource>> m_resources;
    std::vector<SchemaUniformStruct> m_uniformStructs;
    std::vector<SchemaFrameDataUpload> m_frameDataUploads;
    SchemaContext m_context;
    bool m_recording = false;
};

} // namespace lmx::rhi::debug
