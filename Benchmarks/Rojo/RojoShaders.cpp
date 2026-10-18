//----------------------------------------------------------------------------------------------------------------------
/// @file RojoShaders.cpp
/// @brief Implements library loading for the backend this executable links.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoShaders.h"

#include "Host/ExecutableDirectory.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        // Null: the backend's text format, a magic line, the shader ABI version, then one line per entry point. Metal 4:
        // the metallib the Slang rule built beside the executable.
        std::expected<std::vector<std::byte>, std::string> libraryBytes(std::string_view name, std::span<const ShaderEntryName> entries)
        {
#if defined(ROJORHI_BENCH_BACKEND_NULL)
            (void)name;
            std::string text = "rojorhi.null.shaderlibrary\nabi 1\n";
            for (const ShaderEntryName& entry : entries)
            {
                text += std::format("entry {} {}\n", entry.name, entry.stage);
            }
            const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
            return std::vector<std::byte>(bytes, bytes + text.size());
#else
            (void)entries;
            const std::expected<std::filesystem::path, std::string> directory = executableDirectory();
            if (!directory)
            {
                return std::unexpected(directory.error());
            }
            const std::filesystem::path path = *directory / "Shaders" / (std::string(name) + ".metallib");
            std::FILE* file = std::fopen(path.c_str(), "rb");
            if (file == nullptr)
            {
                return std::unexpected(std::format("{} cannot be opened; the rojorhi.slang rule builds it", path.string()));
            }
            std::vector<std::byte> bytes;
            std::array<std::byte, 65536> block;
            size_t read = 0;
            while ((read = std::fread(block.data(), 1, block.size(), file)) > 0)
            {
                bytes.insert(bytes.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(read));
            }
            std::fclose(file);
            return bytes;
#endif
        }
    } // namespace

    //==================================================================================================================
    std::expected<ShaderLibrary, std::string> loadLibrary(Device device, std::string_view name, std::span<const ShaderEntryName> entries)
    {
        const std::expected<std::vector<std::byte>, std::string> bytes = libraryBytes(name, entries);
        if (!bytes)
        {
            return std::unexpected(std::format("library {}: {}", name, bytes.error()));
        }
        const std::string label = std::format("bench.{}", name);
        Result<ShaderLibrary> library = device.loadShaderLibrary(*bytes, label);
        if (!library)
        {
            return std::unexpected(std::format("library {}: {}", name, library.error().message));
        }
        return *library;
    }
} // namespace rojoRHI::bench
