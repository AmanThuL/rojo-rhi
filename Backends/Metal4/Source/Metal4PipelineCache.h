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
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::metal4 {
    /// Returns the blob `Device::pipelineCacheData` hands out for `archives`, at least one, serialized on `device`: the header
    /// lines `rojorhi.metal4.pipelinecache`, `version 1`, `rojorhi <major>.<minor>.<patch>`, `device <name>` and `os <version>`,
    /// then for each archive the line `archive <byte count>` and the archive's bytes; each line ends with one line feed. The
    /// caller holds an autorelease pool.
    std::vector<std::byte> writeBlob(MTL::Device* device, std::span<const std::vector<std::byte>> archives);

    /// Returns the archives' bytes of `blob`, or the reason, as a phrase that completes "the blob ...", it is refused: it was
    /// not written by `writeBlob` for `device`, this OS and this RojoRHI version, it holds no archive, a section's byte count
    /// does not fit the bytes that follow, or bytes after its sections are no section. The caller holds an autorelease pool.
    std::expected<std::vector<std::span<const std::byte>>, std::string_view> archivesOf(std::span<const std::byte> blob, MTL::Device* device);

    /// Returns the per-user temporary directory, read with `confstr(_CS_DARWIN_USER_TEMP_DIR)` rather than from the
    /// environment, or an empty path when the system reports none.
    std::filesystem::path temporaryDirectory();

    /// A file of the temporary directory named `rojorhi-pipelines-<pid>-XXXXXX.metallib`, made unique by `mkstemps` and removed
    /// when the object is destroyed or `remove` is called.
    class TemporaryFile
    {
      public:
        /// Creates an empty file, or returns the reason it could not.
        static Result<TemporaryFile> create();

        TemporaryFile(TemporaryFile&& other) noexcept;
        TemporaryFile& operator=(TemporaryFile&& other) = delete;
        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        /// Removes the file if it still exists.
        ~TemporaryFile();

        /// The file's path.
        const std::filesystem::path& path() const;

        /// Returns the file's path as an autoreleased file URL; the caller holds a pool.
        NS::URL* url() const;

        /// Replaces the file's contents with `bytes`, or returns the reason it could not.
        Result<void> write(std::span<const std::byte> bytes) const;

        /// Returns the file's contents, or the reason they could not be read.
        Result<std::vector<std::byte>> read() const;

        /// Removes the file now.
        void remove();

      private:
        explicit TemporaryFile(std::filesystem::path path);

        std::filesystem::path m_path; ///< The file, or empty once removed or moved from.
    };

    /// A device's pipeline cache: the serializer its compiler records every pipeline into, the archives it serialized so far,
    /// and the archives of the blob it was created with, which creation probes and the compiler looks pipelines up in. The
    /// serializer is set on the compiler descriptor before the compiler is made; the archives are loaded after. The blob only
    /// grows: flushing empties the serializer, so a pipeline created again after a harvest is serialized again, into a later
    /// archive. Each loaded archive costs one `access` and one probe per creation.
    class PipelineCache
    {
      public:
        PipelineCache() = default;
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        /// Creates the serializer on `device`, capturing binaries, or returns why it was not created.
        Result<void> create(MTL::Device* device);

        /// The serializer the compiler descriptor takes.
        MTL4::PipelineDataSetSerializer* serializer() const;

        /// Loads the archives of `blob` on `device`. A blob written for another device, OS or RojoRHI version, or one with an
        /// archive Metal refuses, is one warning through the message callback and leaves the cache without an archive. The
        /// caller holds an autorelease pool; nothing compiles on the device yet.
        void load(MTL::Device* device, std::span<const std::byte> blob);

        /// The task options the next pipeline compiles with, which look pipelines up in the archives; null without one. The
        /// compiler reopens an archive's file at each look-up and aborts when it is gone, so each call first checks that every
        /// file is still readable; once one is not, the first such call logs one warning and every later call returns null.
        const MTL4::CompilerTaskOptions* options();

        /// True when an archive holds the render or mesh pipeline `descriptor`. Called only after `options` returned task
        /// options for the same creation. The caller holds an autorelease pool.
        bool probe(const MTL4::PipelineDescriptor* descriptor) const;

        /// True when an archive holds the compute pipeline `descriptor`, under the same conditions as the render form.
        bool probe(const MTL4::ComputePipelineDescriptor* descriptor) const;

        /// Serializes every pipeline the device's compiler made so far, as a blob for `device`, under the cache's own lock; a
        /// failure is `DriverFailure`. Serializing empties the serializer, so the cache keeps each archive it wrote and the blob
        /// holds them all, the new one last. Free-threaded: it may run beside compilation, whose pipelines may then be missing.
        Result<std::vector<std::byte>> data(MTL::Device* device);

        /// Releases the serializer, the archives and the task options. The caller holds an autorelease pool.
        void release();

      private:
        /// Writes `archive` to a new read-only temporary file, appended to `files`, checks it and opens it as an archive on
        /// `device`, labeled by its index, or returns why the blob is discarded. The caller holds an autorelease pool.
        static Result<MTL4::Archive*> open(MTL::Device* device, std::span<const std::byte> archive, std::vector<TemporaryFile>& files);

        MTL4::PipelineDataSetSerializer* m_serializer = nullptr; ///< Records every pipeline the compiler makes (+1).
        std::vector<MTL4::Archive*> m_archives;                  ///< The archives of the creation blob (+1 each), or none.
        std::atomic<bool> m_lost = false;                        ///< Set once a file of `m_files` was found gone; `options` is then null.
        std::vector<TemporaryFile> m_files;                      ///< The files `m_archives` were loaded from, removed after them.
        MTL4::CompilerTaskOptions* m_options = nullptr;          ///< Look-ups in `m_archives` (+1), or null without one.
        std::vector<std::vector<std::byte>> m_written;           ///< The archives `data` wrote so far, in order.
        std::mutex m_mutex;                                      ///< Serializes `data`, guards `m_written`; its temporary files are its own.
    };
} // namespace rojoRHI::metal4
