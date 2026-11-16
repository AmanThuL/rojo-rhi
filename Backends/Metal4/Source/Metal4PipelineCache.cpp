//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineCache.cpp
/// @brief Implements the Metal 4 pipeline cache: the blob header, the archive's temporary files, the serializer and the archive.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4PipelineCache.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Metal4Common.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rojoRHI::metal4 {
    namespace {
        /// The first line of every blob.
        constexpr std::string_view kMagic = "rojorhi.metal4.pipelinecache";

        /// The second line of every blob: the version of the blob's layout.
        constexpr std::string_view kFormatVersion = "version 1";

        /// The prefix of every temporary file. Files of earlier versions, named `rojorhi-pipelines-<pid>-`, hold no lock, so
        /// `removeAbandonedFiles` never matches them.
        constexpr std::string_view kFilePrefix = "rojorhi-pipelines-locked-";

        /// The age, in seconds, below which an unlocked file is kept: its maker may be between `mkostemps` and `flock`.
        constexpr time_t kAbandonedAge = 60;

        /// The extension of every temporary file: an archive is a metallib, though Metal reads and writes it under any name.
        constexpr std::string_view kFileSuffix = ".metallib";

        //==============================================================================================================
        /// Returns the header lines a blob for `device` starts with, before its first archive line, each ended by a line feed.
        /// The caller holds an autorelease pool.
        std::string headerFor(MTL::Device* device)
        {
            return std::format(
                "{}\n{}\nrojorhi {}.{}.{}\ndevice {}\nos {}\n",
                kMagic,
                kFormatVersion,
                ROJORHI_VERSION_MAJOR,
                ROJORHI_VERSION_MINOR,
                ROJORHI_VERSION_PATCH,
                toStdString(device->name()),
                toStdString(NS::ProcessInfo::processInfo()->operatingSystemVersionString())
            );
        }

        //==============================================================================================================
        /// Returns the system's text for the error number `code`.
        std::string systemMessage(int code)
        {
            return std::error_code(code, std::generic_category()).message();
        }

        //==============================================================================================================
        /// Returns `bytes` as text.
        std::string_view textOf(std::span<const std::byte> bytes)
        {
            return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
        }

        //==============================================================================================================
        /// True when `error`, from serializing, says the serializer recorded nothing since its last flush: the code of a
        /// compilation failure alone is also what a real serialization failure reports, so the description must match too.
        bool nothingRecorded(NS::Error* error)
        {
            return error != nullptr && error->domain()->isEqualToString(MTL::BinaryArchiveDomain) &&
                   error->code() == static_cast<NS::Integer>(MTL::BinaryArchiveErrorCompilationFailure) &&
                   describe(error).find("Nothing to serialize") != std::string::npos;
        }

        //==============================================================================================================
        /// Logs the warning of a blob that is discarded because `reason`.
        void discard(std::string_view reason)
        {
            ROJORHI_LOG_WARN("the pipeline cache blob {} and is discarded; every pipeline is compiled", reason);
        }

        //==============================================================================================================
        /// Removes `path` when no process holds a lock on it, or keeps it; returns 0 then, or the error number of the step that
        /// failed. The file is opened without following a link and removed only while the lock is held and the path still names
        /// the file locked, so a file made again under the name after the directory was read is not removed.
        int removeIfUnlocked(const std::filesystem::path& path)
        {
            const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (descriptor < 0)
            {
                return errno == ENOENT ? 0 : errno;
            }
            int result = 0;
            struct stat opened = {};
            struct stat named = {};
            if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
            {
                result = errno == EWOULDBLOCK ? 0 : errno;
            }
            else if (fstat(descriptor, &opened) != 0)
            {
                result = errno;
            }
            else if (lstat(path.c_str(), &named) != 0)
            {
                result = errno == ENOENT ? 0 : errno;
            }
            else if (opened.st_dev == named.st_dev && opened.st_ino == named.st_ino && unlink(path.c_str()) != 0 && errno != ENOENT)
            {
                result = errno;
            }
            close(descriptor);
            return result;
        }
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> writeBlob(MTL::Device* device, std::span<const std::vector<std::byte>> archives)
    {
        const std::string header = headerFor(device);
        std::vector<std::byte> blob;
        const auto append = [&blob](std::string_view text) {
            const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
            blob.insert(blob.end(), bytes, bytes + text.size());
        };
        append(header);
        for (const std::vector<std::byte>& archive : archives)
        {
            append(std::format("archive {}\n", archive.size()));
            blob.insert(blob.end(), archive.begin(), archive.end());
        }
        return blob;
    }

    //==================================================================================================================
    std::expected<std::vector<std::span<const std::byte>>, std::string_view> archivesOf(std::span<const std::byte> blob, MTL::Device* device)
    {
        const std::string expected = headerFor(device);
        const std::string_view text = textOf(blob);
        if (!text.starts_with(expected))
        {
            return std::unexpected("was not written for this device, OS and RojoRHI version");
        }
        constexpr std::string_view kArchive = "archive ";
        std::vector<std::span<const std::byte>> archives;
        size_t at = expected.size();
        while (at < blob.size())
        {
            const std::string_view rest = text.substr(at);
            const size_t end = rest.find('\n');
            if (!rest.starts_with(kArchive) || end == std::string_view::npos)
            {
                return std::unexpected("has bytes after its archives that are not an archive section");
            }
            const std::string_view count = rest.substr(kArchive.size(), end - kArchive.size());
            uint64_t size = 0;
            const auto [last, error] = std::from_chars(count.data(), count.data() + count.size(), size);
            const size_t start = at + end + 1;
            if (count.empty() || error != std::errc() || last != count.data() + count.size() || size > blob.size() - start)
            {
                return std::unexpected("has an archive section whose byte count does not fit the bytes that follow");
            }
            archives.push_back(blob.subspan(start, static_cast<size_t>(size)));
            at = start + static_cast<size_t>(size);
        }
        if (archives.empty())
        {
            return std::unexpected("holds no archive");
        }
        return archives;
    }

    //==================================================================================================================
    std::filesystem::path temporaryDirectory()
    {
        std::array<char, 1024> path = {};
        const size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, path.data(), path.size());
        if (length == 0 || length > path.size())
        {
            return {};
        }
        return std::filesystem::path(path.data());
    }

    //==================================================================================================================
    Result<TemporaryFile> TemporaryFile::create()
    {
        const std::filesystem::path directory = temporaryDirectory();
        if (directory.empty())
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "the system reports no per-user temporary directory" });
        }
        std::string name = (directory / std::format("{}{}-XXXXXX{}", kFilePrefix, getpid(), kFileSuffix)).string();
        const int descriptor = mkostemps(name.data(), static_cast<int>(kFileSuffix.size()), O_CLOEXEC);
        if (descriptor < 0)
        {
            const int code = errno;
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("no temporary file was made in \"{}\": {}", directory.string(), systemMessage(code)) }
            );
        }
        // The lock is flock's, not fcntl's or lockf's: a POSIX record lock belongs to the process and drops when any
        // descriptor of the file in it closes, and Metal opens and closes the file by its URL. A flock lock belongs to the
        // open file description, so it survives those and still conflicts with another open of the file in this process,
        // which is how a second device's `removeAbandonedFiles` keeps the first device's files.
        if (flock(descriptor, LOCK_SH) != 0)
        {
            const int code = errno;
            unlink(name.c_str());
            close(descriptor);
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the temporary file \"{}\" was not locked: {}", name, systemMessage(code)) }
            );
        }
        return TemporaryFile(std::filesystem::path(name), descriptor);
    }

    //==================================================================================================================
    TemporaryFile::TemporaryFile(std::filesystem::path path, int descriptor) : m_path(std::move(path)), m_descriptor(descriptor)
    {
    }

    //==================================================================================================================
    TemporaryFile::TemporaryFile(TemporaryFile&& other) noexcept : m_path(std::exchange(other.m_path, {})), m_descriptor(std::exchange(other.m_descriptor, -1))
    {
    }

    //==================================================================================================================
    TemporaryFile::~TemporaryFile()
    {
        remove();
    }

    //==================================================================================================================
    const std::filesystem::path& TemporaryFile::path() const
    {
        return m_path;
    }

    //==================================================================================================================
    NS::URL* TemporaryFile::url() const
    {
        return NS::URL::fileURLWithPath(makeString(m_path.string()));
    }

    //==================================================================================================================
    Result<void> TemporaryFile::write(std::span<const std::byte> bytes) const
    {
        std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the temporary file \"{}\" was not written", m_path.string()) });
        }
        return {};
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> TemporaryFile::read() const
    {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(m_path, error);
        std::vector<std::byte> bytes(error ? 0 : static_cast<size_t>(size));
        std::ifstream stream(m_path, std::ios::binary);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (error || !stream)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the temporary file \"{}\" was not read", m_path.string()) });
        }
        return bytes;
    }

    //==================================================================================================================
    void TemporaryFile::remove()
    {
        // Removed before the lock is released, so no sweep finds the file unlocked under its name.
        if (!m_path.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            m_path.clear();
        }
        if (m_descriptor >= 0)
        {
            close(std::exchange(m_descriptor, -1));
        }
    }

    //==================================================================================================================
    void removeAbandonedFiles()
    {
        const std::filesystem::path directory = temporaryDirectory();
        if (directory.empty())
        {
            ROJORHI_LOG_WARN("abandoned pipeline cache files were not looked for: the system reports no per-user temporary directory");
            return;
        }
        std::error_code error;
        std::filesystem::directory_iterator entries(directory, error);
        if (error)
        {
            ROJORHI_LOG_WARN("abandoned pipeline cache files were not looked for in \"{}\": {}", directory.string(), error.message());
            return;
        }
        const time_t now = time(nullptr);
        size_t failed = 0;
        std::string first;
        for (const std::filesystem::directory_entry& entry : entries)
        {
            const std::string name = entry.path().filename().string();
            struct stat status = {};
            if (!name.starts_with(kFilePrefix) || !name.ends_with(kFileSuffix) || lstat(entry.path().c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
                now - status.st_mtimespec.tv_sec < kAbandonedAge)
            {
                continue;
            }
            if (const int code = removeIfUnlocked(entry.path()); code != 0)
            {
                if (failed++ == 0)
                {
                    first = std::format("\"{}\": {}", entry.path().string(), systemMessage(code));
                }
            }
        }
        if (failed != 0)
        {
            ROJORHI_LOG_WARN("{} abandoned pipeline cache file(s) were not removed; the first, {}", failed, first);
        }
    }

    //==================================================================================================================
    Result<void> PipelineCache::create(MTL::Device* device)
    {
        MTL4::PipelineDataSetSerializerDescriptor* descriptor = MTL4::PipelineDataSetSerializerDescriptor::alloc()->init();
        descriptor->setConfiguration(MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
        m_serializer = device->newPipelineDataSetSerializer(descriptor);
        descriptor->release();
        if (m_serializer == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, "the pipeline data set serializer was not created" });
        }
        return {};
    }

    //==================================================================================================================
    MTL4::PipelineDataSetSerializer* PipelineCache::serializer() const
    {
        return m_serializer;
    }

    //==================================================================================================================
    void PipelineCache::load(MTL::Device* device, std::span<const std::byte> blob)
    {
        if (blob.empty())
        {
            return;
        }
        const std::expected<std::vector<std::span<const std::byte>>, std::string_view> archives = archivesOf(blob, device);
        if (!archives)
        {
            discard(archives.error());
            return;
        }
        std::vector<MTL4::Archive*> opened;
        std::vector<TemporaryFile> files;
        for (const std::span<const std::byte> archive : *archives)
        {
            Result<MTL4::Archive*> made = open(device, archive, files);
            if (!made)
            {
                for (MTL4::Archive* refused : opened)
                {
                    refused->release();
                }
                discard(made.error().message);
                return;
            }
            opened.push_back(*made);
        }
        // The compiler reads an archive's file again at each look-up, so the files live as long as the archives.
        m_archives = std::move(opened);
        m_files = std::move(files);
        m_options = MTL4::CompilerTaskOptions::alloc()->init();
        m_options->setLookupArchives(
            NS::Array::array(reinterpret_cast<const NS::Object* const*>(m_archives.data()), static_cast<NS::UInteger>(m_archives.size()))
        );
    }

    //==================================================================================================================
    Result<MTL4::Archive*> PipelineCache::open(MTL::Device* device, std::span<const std::byte> archive, std::vector<TemporaryFile>& files)
    {
        Result<TemporaryFile> file = TemporaryFile::create();
        if (!file)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("could not be written to a temporary file: {}", file.error().message) });
        }
        if (Result<void> written = file->write(archive); !written)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("could not be written to a temporary file: {}", written.error().message) });
        }
        // Read-only, so nothing of this process rewrites the archive under the compiler.
        if (chmod(file->path().c_str(), S_IRUSR) != 0)
        {
            const int code = errno;
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure,
                       std::format("could not be written to a temporary file: \"{}\" was not made read-only: {}", file->path().string(), systemMessage(code)) }
            );
        }
        // Metal opens a Metal 4 archive without reading it, and its compiler later reads the file as a binary archive and
        // throws when it cannot. Opening it as a binary archive first refuses a damaged archive here, as a warning.
        NS::Error* error = nullptr;
        MTL::BinaryArchiveDescriptor* check = MTL::BinaryArchiveDescriptor::alloc()->init();
        check->setUrl(file->url());
        MTL::BinaryArchive* readable = device->newBinaryArchive(check, &error);
        check->release();
        if (readable == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("holds an archive Metal refused ({})", describe(error)) });
        }
        readable->release();
        error = nullptr;
        MTL4::Archive* opened = device->newArchive(file->url(), &error);
        if (opened == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("holds an archive Metal refused ({})", describe(error)) });
        }
        opened->setLabel(makeString(std::format("rojorhi.pipelineCache.{}", files.size())));
        files.push_back(std::move(*file));
        return opened;
    }

    //==================================================================================================================
    const MTL4::CompilerTaskOptions* PipelineCache::options()
    {
        if (m_options == nullptr || m_lost.load(std::memory_order_relaxed))
        {
            return nullptr;
        }
        for (const TemporaryFile& file : m_files)
        {
            if (access(file.path().c_str(), R_OK) != 0)
            {
                const int code = errno;
                if (!m_lost.exchange(true, std::memory_order_relaxed))
                {
                    ROJORHI_LOG_WARN(
                        "the pipeline cache's archive file \"{}\" can no longer be read ({}); every later pipeline is compiled without the cache",
                        file.path().string(),
                        systemMessage(code)
                    );
                }
                return nullptr;
            }
        }
        return m_options;
    }

    //==================================================================================================================
    bool PipelineCache::probe(const MTL4::PipelineDescriptor* descriptor) const
    {
        for (MTL4::Archive* archive : m_archives)
        {
            NS::Error* error = nullptr;
            if (MTL::RenderPipelineState* state = archive->newRenderPipelineState(descriptor, &error); state != nullptr)
            {
                state->release();
                return true;
            }
        }
        return false;
    }

    //==================================================================================================================
    bool PipelineCache::probe(const MTL4::ComputePipelineDescriptor* descriptor) const
    {
        for (MTL4::Archive* archive : m_archives)
        {
            NS::Error* error = nullptr;
            if (MTL::ComputePipelineState* state = archive->newComputePipelineState(descriptor, &error); state != nullptr)
            {
                state->release();
                return true;
            }
        }
        return false;
    }

    //==================================================================================================================
    std::shared_lock<std::shared_mutex> PipelineCache::compiling()
    {
        return std::shared_lock(m_mutex);
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> PipelineCache::data(MTL::Device* device)
    {
        const AutoreleasePool pool;
        // Exclusive: flushing the serializer while the compiler records a pipeline into it crashes inside Metal.
        const std::unique_lock lock(m_mutex);
        Result<TemporaryFile> file = TemporaryFile::create();
        if (!file)
        {
            return std::unexpected(std::move(file.error()));
        }
        NS::Error* error = nullptr;
        if (!m_serializer->serializeAsArchiveAndFlushToURL(file->url(), &error))
        {
            // Metal refuses a serializer that recorded nothing since its last flush as a compilation failure ("Nothing to
            // serialize"); the archives written so far then cover every pipeline.
            if (!m_written.empty() && nothingRecorded(error))
            {
                return writeBlob(device, m_written);
            }
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the pipeline cache was not serialized: {}", describe(error)) });
        }
        Result<std::vector<std::byte>> archive = file->read();
        if (!archive)
        {
            return std::unexpected(std::move(archive.error()));
        }
        // The serializer now holds nothing of what it wrote, so the blob carries every archive written so far.
        m_written.push_back(std::move(*archive));
        return writeBlob(device, m_written);
    }

    //==================================================================================================================
    void PipelineCache::release()
    {
        if (m_options != nullptr)
        {
            m_options->release();
            m_options = nullptr;
        }
        for (MTL4::Archive* archive : m_archives)
        {
            archive->release();
        }
        m_archives.clear();
        m_files.clear();
        m_written.clear();
        if (m_serializer != nullptr)
        {
            m_serializer->release();
            m_serializer = nullptr;
        }
    }
} // namespace rojoRHI::metal4
