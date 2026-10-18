//----------------------------------------------------------------------------------------------------------------------
/// @file Outputs.h
/// @brief Declares what the harness does with read-back outputs: hash them, dump them and compare two arms' sets.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    // Lowercase hexadecimal SHA-256 of the bytes.
    std::string sha256Hex(std::span<const std::byte> bytes);
    // Writes each output to `directory/<id>.<arm>.<output name>.bin`, creating the directory.
    std::expected<void, std::string> dumpOutputs(const std::filesystem::path& directory, std::string_view id, Arm arm, std::span<const Output> outputs);
    // True when both sets hold the same names in the same order with identical bytes.
    bool sameOutputs(std::span<const Output> first, std::span<const Output> second);
} // namespace rojoRHI::bench
