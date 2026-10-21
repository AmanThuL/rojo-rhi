//----------------------------------------------------------------------------------------------------------------------
/// @file Results.cpp
/// @brief Implements the run JSON of the harness contract through JsonWriter.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Results.h"

#include "Common/CounterFields.h"
#include "Common/JsonWriter.h"
#include "Common/Outputs.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    namespace {
#if defined(NDEBUG)
        constexpr std::string_view kBuild = "release";
#else
        constexpr std::string_view kBuild = "debug";
#endif
#if defined(ROJORHI_BENCH_BACKEND_METAL4)
        constexpr std::string_view kBackend = "metal4";
#elif defined(ROJORHI_BENCH_BACKEND_D3D12)
        constexpr std::string_view kBackend = "d3d12";
#else
        constexpr std::string_view kBackend = "null";
#endif

        //==============================================================================================================
        const ArmResult* armOf(std::span<const ArmResult> arms, Arm arm)
        {
            for (const ArmResult& result : arms)
            {
                if (result.arm == arm)
                {
                    return &result;
                }
            }
            return nullptr;
        }

        //==============================================================================================================
        void writeOptionalNumbers(JsonWriter& json, std::string_view name, const std::vector<std::optional<double>>& values)
        {
            json.key(name).beginArray();
            for (const std::optional<double>& value : values)
            {
                if (value)
                {
                    json.number(*value);
                }
                else
                {
                    json.null();
                }
            }
            json.endArray();
        }

        //==============================================================================================================
        void writeCounts(JsonWriter& json, std::string_view name, const std::vector<uint64_t>& values)
        {
            json.key(name).beginArray();
            for (const uint64_t value : values)
            {
                json.unsignedInteger(value);
            }
            json.endArray();
        }

        //==============================================================================================================
        void writeCounters(JsonWriter& json, const Counters& counters)
        {
            json.beginObject();
            for (const CounterField& field : kCounterFields)
            {
                json.key(field.name).unsignedInteger(counters.*field.member);
            }
            json.endObject();
        }

        //==============================================================================================================
        void writeArm(JsonWriter& json, const ArmResult& arm)
        {
            // A hash that cannot be computed fails the arm, so two failing arms never agree on a hash.
            std::vector<std::string> hashes;
            std::string hashError;
            for (const Output& output : arm.outputs)
            {
                std::expected<std::string, std::string> hash = sha256Hex(output.bytes);
                if (!hash && hashError.empty())
                {
                    hashError = std::format("output {}: {}", output.name, hash.error());
                }
                hashes.push_back(hash.value_or(std::string()));
            }
            json.beginObject();
            json.key("status").string(arm.ok && hashError.empty() ? "ok" : "failed");
            json.key("error").string(arm.ok ? hashError : arm.error);
            writeOptionalNumbers(json, "cpuNs", arm.cpuNs);
            writeOptionalNumbers(json, "gpuNs", arm.gpuNs);
            writeCounts(json, "allocations", arm.allocations);
            writeCounts(json, "nativeCalls", arm.nativeCalls);
            writeCounts(json, "nativeBarriers", arm.nativeBarriers);
            if (arm.cpuAllocations)
            {
                writeCounts(json, "cpuAllocations", *arm.cpuAllocations);
            }
            else
            {
                json.key("cpuAllocations").null();
            }
            json.key("counters");
            if (arm.countersAfterWarmup && arm.countersEnd)
            {
                json.beginObject();
                json.key("afterWarmup");
                writeCounters(json, *arm.countersAfterWarmup);
                json.key("end");
                writeCounters(json, *arm.countersEnd);
                json.endObject();
            }
            else
            {
                json.null();
            }
            json.key("frameCounters");
            if (arm.frameCounters)
            {
                writeCounters(json, *arm.frameCounters);
            }
            else
            {
                json.null();
            }
            json.key("outputs").beginArray();
            for (size_t index = 0; index < arm.outputs.size(); ++index)
            {
                const Output& output = arm.outputs[index];
                json.beginObject();
                json.key("name").string(output.name);
                json.key("bytes").unsignedInteger(output.bytes.size());
                json.key("sha256").string(hashes[index]);
                json.endObject();
            }
            json.endArray();
            json.endObject();
        }

        //==============================================================================================================
        void writeComparison(JsonWriter& json, std::string_view name, std::optional<bool> equal)
        {
            json.key(name);
            if (equal)
            {
                json.boolean(*equal);
            }
            else
            {
                json.null();
            }
        }
    } // namespace

    //==================================================================================================================
    std::string runJson(const RunRecord& record)
    {
        JsonWriter json;
        json.beginObject();
        json.key("schema").integer(1);
        json.key("id").string(record.config.id);
        json.key("workload").string(record.config.workload);
        json.key("params");
        writeParams(json, record.config);
        json.key("mode").string(modeName(record.mode));
        json.key("build").string(kBuild);
        json.key("backend").string(kBackend);
        json.key("validation").boolean(record.validation);
        json.key("order").beginArray();
        for (const ArmResult& arm : record.arms)
        {
            json.string(armName(arm.arm));
        }
        json.endArray();
        json.key("device").string(record.device);
        json.key("os").string(record.os);
        json.key("thermalStart").string(record.thermalStart);
        json.key("thermalEnd").string(record.thermalEnd);
        json.key("arms").beginObject();
        for (const ArmResult& arm : record.arms)
        {
            json.key(armName(arm.arm));
            writeArm(json, arm);
        }
        json.endObject();
        writeComparison(json, "outputsEqual", outputsEqual(record.arms, Arm::Rojo, Arm::Native));
        if (record.config.hasArm(Arm::Fixed))
        {
            writeComparison(json, "outputsEqualF", outputsEqual(record.arms, Arm::Fixed, Arm::Native));
        }
        json.endObject();
        return json.text();
    }

    //==================================================================================================================
    std::optional<bool> outputsEqual(std::span<const ArmResult> arms, Arm first, Arm second)
    {
        const ArmResult* a = armOf(arms, first);
        const ArmResult* b = armOf(arms, second);
        if (a == nullptr || b == nullptr || !a->ok || !b->ok)
        {
            return std::nullopt;
        }
        return sameOutputs(a->outputs, b->outputs);
    }
} // namespace rojoRHI::bench
