//----------------------------------------------------------------------------------------------------------------------
/// @file RojoShaders.cpp
/// @brief Implements library loading for the backend this executable links.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoShaders.h"

#include "Host/ExecutableDirectory.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        //==============================================================================================================
        // Null: the backend's text format, a magic line, the shader ABI version, then one line per entry point. Metal 4 and
        // D3D12: the .metal4lib or .d3d12lib the Slang rule built beside the executable.
        std::expected<std::vector<std::byte>, std::string> libraryBytes(std::string_view name, std::span<const ShaderEntryName> entries)
        {
#if defined(ROJORHI_BENCH_BACKEND_NULL)
            (void)name;
            std::string text = "rojorhi.null.shaderlibrary\nabi 2\n";
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
#if defined(ROJORHI_BENCH_BACKEND_D3D12)
            constexpr std::string_view kExtension = ".d3d12lib";
#else
            constexpr std::string_view kExtension = ".metal4lib";
#endif
            const std::filesystem::path path = *directory / "Shaders" / (std::string(name) + std::string(kExtension));
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::unexpected(std::format("{} cannot be opened; the rojorhi.slang rule builds it", path.string()));
            }
            std::vector<char> text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            if (file.bad())
            {
                return std::unexpected(std::format("{} cannot be read", path.string()));
            }
            const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
            return std::vector<std::byte>(bytes, bytes + text.size());
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
