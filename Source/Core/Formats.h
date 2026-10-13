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

    /// Issues VU-rojo-format-0001 when `format` is not an enumerator and VU-rojo-format-0005 when it is `Format::Undefined`.
    void checkQueryFormat(Format format);
} // namespace rojoRHI::core
