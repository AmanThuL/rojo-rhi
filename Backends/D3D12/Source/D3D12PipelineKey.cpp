//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineKey.cpp
/// @brief Implements the D3D12 pipeline cache's key and blob header, which include no Windows header.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12PipelineKey.h"

#include <rojoRHI/RHI.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        /// The first line of every blob.
        constexpr std::string_view kMagic = "rojorhi.d3d12.pipelinecache";

        /// The second line of every blob: the version of the blob's layout.
        constexpr std::string_view kFormatVersion = "version 1";

        /// The start of the fourth line, which counts the library bytes after it.
        constexpr std::string_view kLibrary = "library ";

        /// The kind a key starts with.
        enum class KeyKind : uint8_t
        {
            Vertex,
            Mesh,
            Compute,
        };

        /// The first byte of a DXIL container's digest and the digest's length.
        constexpr size_t kDigestOffset = 4;
        constexpr size_t kDigestSize = 16;

        //==============================================================================================================
        /// Appends the low `width` bytes of `value` to `bytes`, least significant first.
        void put(std::vector<std::byte>& bytes, uint64_t value, size_t width)
        {
            for (size_t i = 0; i < width; ++i)
            {
                bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
            }
        }

        //==============================================================================================================
        /// Appends one byte holding `value`.
        void put8(std::vector<std::byte>& bytes, uint64_t value)
        {
            put(bytes, value, 1);
        }

        //==============================================================================================================
        /// Appends four bytes holding `value`.
        void put32(std::vector<std::byte>& bytes, uint64_t value)
        {
            put(bytes, value, 4);
        }

        //==============================================================================================================
        /// Appends a byte count and then `data`.
        void putCounted(std::vector<std::byte>& bytes, std::span<const std::byte> data)
        {
            put32(bytes, data.size());
            bytes.insert(bytes.end(), data.begin(), data.end());
        }

        //==============================================================================================================
        /// Appends one stage: a presence byte and, for a present stage, its entry `name` and the digest of `dxil`, bytes 4 to
        /// 19 of the container. A container too short to hold a digest, which the shader library's reader refuses, keeps
        /// whatever bytes it has past the magic.
        void putStage(std::vector<std::byte>& bytes, std::string_view name, std::span<const std::byte> dxil)
        {
            put8(bytes, dxil.empty() ? 0 : 1);
            if (dxil.empty())
            {
                return;
            }
            putCounted(bytes, std::as_bytes(std::span(name.data(), name.size())));
            const size_t start = std::min(kDigestOffset, dxil.size());
            putCounted(bytes, dxil.subspan(start, std::min(kDigestSize, dxil.size() - start)));
        }

        //==============================================================================================================
        /// Appends the stencil operations and comparison of one face.
        void putFace(std::vector<std::byte>& bytes, const StencilFaceState& face)
        {
            put8(bytes, static_cast<uint8_t>(face.fail));
            put8(bytes, static_cast<uint8_t>(face.depthFail));
            put8(bytes, static_cast<uint8_t>(face.pass));
            put8(bytes, static_cast<uint8_t>(face.compare));
        }

        //==============================================================================================================
        /// Returns the line of `text` that starts at `at` without its line feed, and moves `at` past the line feed; nothing
        /// when no line feed ends it.
        std::optional<std::string_view> nextLine(std::string_view text, size_t& at)
        {
            const size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            const std::string_view line = text.substr(at, end - at);
            at = end + 1;
            return line;
        }
    } // namespace

    //==================================================================================================================
    std::array<uint64_t, 2> pipelineKey(std::span<const std::byte> canonical)
    {
        // The prime is 2^88 + 0x13b: multiplying by it adds the low limb shifted up 88 bits to the product with 0x13b.
        constexpr uint64_t kPrimeLow = 0x13b;
        uint64_t high = 0x6c62272e07bb0142;
        uint64_t low = 0x62b821756295c58d;
        for (const std::byte byte : canonical)
        {
            low ^= static_cast<uint64_t>(byte);
            // The 128-bit product of the low limb and 0x13b, from its two 32-bit halves.
            const uint64_t lowHalf = (low & 0xFFFFFFFF) * kPrimeLow;
            const uint64_t highHalf = (low >> 32) * kPrimeLow + (lowHalf >> 32);
            const uint64_t productLow = (highHalf << 32) | (lowHalf & 0xFFFFFFFF);
            const uint64_t productHigh = highHalf >> 32;
            high = productHigh + high * kPrimeLow + (low << 24);
            low = productLow;
        }
        return { high, low };
    }

    //==================================================================================================================
    std::wstring keyName(std::array<uint64_t, 2> key)
    {
        return std::format(L"{:016x}{:016x}", key[0], key[1]);
    }

    //==================================================================================================================
    std::vector<std::byte> canonicalKey(const GraphicsPipelineDesc& desc, const StageDxil& dxil)
    {
        const bool mesh = !dxil.mesh.empty();
        std::vector<std::byte> bytes;
        bytes.reserve(128 + 16 * desc.colors.size());
        put8(bytes, static_cast<uint8_t>(mesh ? KeyKind::Mesh : KeyKind::Vertex));
        putStage(bytes, desc.vertex.name, dxil.vertex);
        putStage(bytes, desc.task.name, dxil.task);
        putStage(bytes, desc.mesh.name, dxil.mesh);
        putStage(bytes, desc.fragment.name, dxil.fragment);
        if (!mesh)
        {
            put8(bytes, static_cast<uint8_t>(desc.primitive));
        }
        const RasterState& raster = desc.raster;
        put8(bytes, static_cast<uint8_t>(raster.cull));
        put8(bytes, static_cast<uint8_t>(raster.frontFace));
        put8(bytes, static_cast<uint8_t>(raster.fill));
        put8(bytes, raster.depthClip ? 1 : 0);
        put8(bytes, raster.depthBias ? 1 : 0);
        put8(bytes, raster.conservative ? 1 : 0);
        const DepthStencilState& stencil = desc.depthStencil;
        put8(bytes, stencil.depthTest ? 1 : 0);
        put8(bytes, stencil.depthWrite ? 1 : 0);
        put8(bytes, static_cast<uint8_t>(stencil.depthCompare));
        put8(bytes, stencil.stencilTest ? 1 : 0);
        put8(bytes, stencil.stencilReadMask);
        put8(bytes, stencil.stencilWriteMask);
        putFace(bytes, stencil.front);
        putFace(bytes, stencil.back);
        put32(bytes, desc.colors.size());
        for (const ColorTarget& target : desc.colors)
        {
            put32(bytes, static_cast<uint32_t>(target.format));
            const BlendState& blend = target.blend;
            put8(bytes, blend.enable ? 1 : 0);
            put8(bytes, static_cast<uint8_t>(blend.srcColor));
            put8(bytes, static_cast<uint8_t>(blend.dstColor));
            put8(bytes, static_cast<uint8_t>(blend.colorOp));
            put8(bytes, static_cast<uint8_t>(blend.srcAlpha));
            put8(bytes, static_cast<uint8_t>(blend.dstAlpha));
            put8(bytes, static_cast<uint8_t>(blend.alphaOp));
            put8(bytes, static_cast<uint8_t>(target.writeMask));
        }
        put32(bytes, static_cast<uint32_t>(desc.depthFormat));
        put32(bytes, desc.samples);
        return bytes;
    }

    //==================================================================================================================
    std::vector<std::byte> canonicalKey(const ComputePipelineDesc& desc, std::span<const std::byte> dxil)
    {
        std::vector<std::byte> bytes;
        put8(bytes, static_cast<uint8_t>(KeyKind::Compute));
        putStage(bytes, desc.compute.name, dxil);
        return bytes;
    }

    //==================================================================================================================
    std::string blobHeader(size_t libraryBytes)
    {
        return std::format(
            "{}\n{}\nrojorhi {}.{}.{}\n{}{}\n",
            kMagic,
            kFormatVersion,
            ROJORHI_VERSION_MAJOR,
            ROJORHI_VERSION_MINOR,
            ROJORHI_VERSION_PATCH,
            kLibrary,
            libraryBytes
        );
    }

    //==================================================================================================================
    std::expected<std::span<const std::byte>, std::string_view> libraryOf(std::span<const std::byte> blob)
    {
        const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
        size_t at = 0;
        if (nextLine(text, at) != kMagic)
        {
            return std::unexpected("was not written by the D3D12 backend");
        }
        if (nextLine(text, at) != kFormatVersion)
        {
            return std::unexpected("has another blob version");
        }
        const std::string version = std::format("rojorhi {}.{}.{}", ROJORHI_VERSION_MAJOR, ROJORHI_VERSION_MINOR, ROJORHI_VERSION_PATCH);
        if (nextLine(text, at) != version)
        {
            return std::unexpected("was written by another RojoRHI version");
        }
        constexpr std::string_view kMiscounted = "has a library line that does not count the bytes after it";
        const std::optional<std::string_view> line = nextLine(text, at);
        if (!line || !line->starts_with(kLibrary))
        {
            return std::unexpected(kMiscounted);
        }
        const std::string_view count = line->substr(kLibrary.size());
        uint64_t size = 0;
        const auto [last, error] = std::from_chars(count.data(), count.data() + count.size(), size);
        if (count.empty() || error != std::errc() || last != count.data() + count.size() || size != blob.size() - at)
        {
            return std::unexpected(kMiscounted);
        }
        return blob.subspan(at);
    }
} // namespace rojoRHI::d3d12
