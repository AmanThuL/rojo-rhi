//----------------------------------------------------------------------------------------------------------------------
/// @file Formats.h
/// @brief Declares the per-class capability rules of chapter 15 and the table a device keeps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Format.h>
#include <rojoRHI/FormatCapabilities.h>

#include <array>
#include <cstdint>

namespace rojoRHI::core {
    /// The number of `Format` enumerators, `Undefined` included; the length of a device's capability table.
    inline constexpr uint32_t kFormatCount = static_cast<uint32_t>(Format::RGB10A2Unorm) + 1;

    /// One entry per `Format`, indexed by its value.
    using FormatTable = std::array<FormatCapabilities, kFormatCount>;

    /// Returns the most a device can report for `format`: every capability the reference allows the format's class. A backend
    /// reports the intersection of this with what its hardware supports, never more. `Format::Undefined` has none.
    FormatCapabilities formatCeiling(Format format);

    /// Returns `formatCeiling` for every format: the table the Null backend reports.
    FormatTable formatCeilings();

    /// How a format packs texels: a block of `blockWidth` by `blockHeight` texels occupies `blockBytes` bytes. An uncompressed
    /// format has a 1 by 1 block; `Format::Undefined` and `D32FloatS8` have no block bytes (0).
    struct FormatLayout
    {
        uint32_t blockWidth = 1;  ///< Texels of one block along x.
        uint32_t blockHeight = 1; ///< Texels of one block along y.
        uint32_t blockBytes = 0;  ///< Bytes of one block; `D32FloatS8` has none, because the reference gives it no packed size.
    };

    /// Returns how `format` packs texels.
    FormatLayout formatLayout(Format format);

    /// Returns true for a block-compressed format.
    bool isBlockCompressed(Format format);

    /// Returns true for a format with a depth plane, with or without stencil.
    bool isDepthFormat(Format format);

    /// Returns the format that differs from `format` only in sRGB transfer, or `Format::Undefined` when it has no such partner.
    Format srgbCounterpart(Format format);

    /// Issues VU-rojo-format-0001 when `format` is not an enumerator and VU-rojo-format-0005 when it is `Format::Undefined`.
    void checkQueryFormat(Format format);
} // namespace rojoRHI::core
