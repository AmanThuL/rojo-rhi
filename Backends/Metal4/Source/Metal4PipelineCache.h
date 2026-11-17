//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineCache.h
/// @brief Declares the Metal 4 pipeline cache: the blob around an archive, its temporary files, the serializer and the archive.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::metal4 {
    // Each line ends with one line feed. The caller holds an autorelease pool.
    std::vector<std::byte> writeBlob(MTL::Device* device, std::span<const std::vector<std::byte>> archives);

    // The reason completes the phrase "the blob ...". The caller holds an autorelease pool.
    std::expected<std::vector<std::span<const std::byte>>, std::string_view> archivesOf(std::span<const std::byte> blob, MTL::Device* device);

    // Read with `confstr(_CS_DARWIN_USER_TEMP_DIR)` rather than from the environment; empty when the system reports
    // none.
    std::filesystem::path temporaryDirectory();

    // Removes only unlocked files at least a minute old, since a younger one's maker may not have locked it yet. A
    // failure is one warning and never more.
    void removeAbandonedFiles();

    // Held open with `flock(LOCK_SH)` for the object's lifetime so that `removeAbandonedFiles` in any process keeps it;
    // removed, then closed, on destruction or `remove`.
    class TemporaryFile
    {
      public:
        static Result<TemporaryFile> create();

        TemporaryFile(TemporaryFile&& other) noexcept;
        TemporaryFile& operator=(TemporaryFile&& other) = delete;
        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        ~TemporaryFile();

        const std::filesystem::path& path() const;

        // Autoreleased; the caller holds a pool.
        NS::URL* url() const;

        Result<void> write(std::span<const std::byte> bytes) const;

        Result<std::vector<std::byte>> read() const;

        void remove();

      private:
        TemporaryFile(std::filesystem::path path, int descriptor);

        std::filesystem::path m_path; // Empty once removed or moved from.
        int m_descriptor = -1;        // Holds the file's shared lock; -1 once removed or moved from.
    };

    // The serializer is set on the compiler descriptor before the compiler is made; the archives are loaded after. The
    // blob only grows: flushing empties the serializer, so a pipeline created again after a harvest is serialized
    // again, into a later archive. Each loaded archive costs one `access` and one probe per creation.
    class PipelineCache
    {
      public:
        PipelineCache() = default;
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        Result<void> create(MTL::Device* device);

        MTL4::PipelineDataSetSerializer* serializer() const;

        // A blob written for another device, OS or RojoRHI version, or with an archive Metal refuses, is one warning
        // and leaves the cache without an archive. The caller holds an autorelease pool; nothing compiles on the device
        // yet.
        void load(MTL::Device* device, std::span<const std::byte> blob);

        // The compiler reopens an archive's file at each look-up and aborts when it is gone, so each call first checks
        // that every file is still readable; once one is not, it warns once and returns null from then on.
        const MTL4::CompilerTaskOptions* options();

        // Called only after `options` returned task options for the same creation. The caller holds an autorelease
        // pool.
        bool probe(const MTL4::PipelineDescriptor* descriptor) const;

        bool probe(const MTL4::ComputePipelineDescriptor* descriptor) const;

        // Held shared while the compiler makes a pipeline: Metal's serializer crashes when it is flushed while a
        // compilation records into it.
        [[nodiscard]] std::shared_lock<std::shared_mutex> compiling();

        // Serializing empties the serializer, so the cache keeps each archive it wrote and the blob holds them all, the
        // new one last. Waits for the compilations in flight, which the blob then covers; one that starts meanwhile
        // waits for it.
        Result<std::vector<std::byte>> data(MTL::Device* device);

        // The caller holds an autorelease pool.
        void release();

      private:
        // The caller holds an autorelease pool.
        static Result<MTL4::Archive*> open(MTL::Device* device, std::span<const std::byte> archive, std::vector<TemporaryFile>& files);

        MTL4::PipelineDataSetSerializer* m_serializer = nullptr; // +1.
        std::vector<MTL4::Archive*> m_archives;                  // +1 each.
        std::atomic<bool> m_lost = false;                        // Once set, `options` returns null.
        std::vector<TemporaryFile> m_files;                      // Removed after the archives.
        MTL4::CompilerTaskOptions* m_options = nullptr;          // +1, or null without an archive.
        std::vector<std::vector<std::byte>> m_written;
        std::shared_mutex m_mutex; // Held by `data` exclusively, guarding `m_written`, and shared by `compiling`'s holders.
    };
} // namespace rojoRHI::metal4
