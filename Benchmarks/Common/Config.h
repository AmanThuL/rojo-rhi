//----------------------------------------------------------------------------------------------------------------------
/// @file Config.h
/// @brief Declares one benchmark configuration, the arms that measure it and its JSON forms.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    class JsonWriter;

    // R records through RHI.h, N is hand-written metal-cpp, F is G2's fixed-slot native arm.
    enum class Arm : uint8_t
    {
        Rojo,
        Native,
        Fixed,
    };

    // "R", "N" or "F".
    std::string_view armName(Arm arm);
    std::optional<Arm> armFromName(std::string_view name);

    // A workload at one set of parameters, named by its id. A parameter the workload does not take stays empty and is
    // left out of the params object.
    struct Config
    {
        std::string id; ///< Such as "C7-k100".
        std::string workload;
        std::optional<uint32_t> k; ///< Repetitions per frame.
        std::optional<uint32_t> maxCount;
        std::optional<bool> nullCount;
        std::optional<std::string> pattern; ///< "uniform" or "divergent".
        std::vector<Arm> arms;              ///< The arms that exist, in R, N, F order.

        bool hasArm(Arm arm) const;
    };

    // The params object: "K", "maxCount", "nullCount" and "pattern", each when set.
    void writeParams(JsonWriter& json, const Config& config);
    // One line of `RojoRHIBench list`: {"id", "workload", "params", "arms"}, with no newline.
    std::string listJson(const Config& config);
} // namespace rojoRHI::bench
