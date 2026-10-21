//----------------------------------------------------------------------------------------------------------------------
/// @file Main.cpp
/// @brief Entry point of RojoRHIBench: installs the allocation counter, then dispatches the command line.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/Outputs.h"
#include "Common/Registry.h"
#include "Common/Results.h"
#include "Common/RunOptions.h"
#include "Host/AllocationCounter.h"
#include "Host/Environment.h"
#include "Host/ThermalState.h"

#include <catch2/catch_session.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace rojoRHI::bench;

    // Exit status of a malformed command line; a harness failure returns 1.
    constexpr int kUsageError = 2;

    // A command receives the program name and the arguments after the command's own name.
    struct Command
    {
        std::string_view name;
        std::string_view usage;
        int (*run)(char* program, std::span<char*> arguments);
    };

    //==================================================================================================================
    int runEnvironment(char* /*program*/, std::span<char*> arguments)
    {
        if (arguments.size() != 1)
        {
            std::fprintf(stderr, "environment: expected one argument, OUT.json\n");
            return kUsageError;
        }
        const auto written = writeEnvironmentFile(arguments[0]);
        if (!written)
        {
            std::fprintf(stderr, "%s\n", written.error().c_str());
            return 1;
        }
        return 0;
    }

    //==================================================================================================================
    int runList(char* /*program*/, std::span<char*> arguments)
    {
        if (!arguments.empty())
        {
            std::fprintf(stderr, "list: takes no argument\n");
            return kUsageError;
        }
        for (const WorkloadEntry& entry : registry())
        {
            std::printf("%s\n", listJson(entry.config).c_str());
        }
        return 0;
    }

    // The parsed arguments of `run`.
    struct RunArguments
    {
        const WorkloadEntry* entry = nullptr;
        RunOptions options;
        std::vector<Arm> order; ///< Arms to run, in running order.
        std::string out;
    };

    //==================================================================================================================
    int usageError(const std::string& message)
    {
        std::fprintf(stderr, "run: %s\n", message.c_str());
        return kUsageError;
    }

    //==================================================================================================================
    // Parses `ID --mode correctness|timing --order RN|NR --arms R,N[,F] --out OUT.json [--dump DIR] [--validation]`. R and N
    // run in --order, F always last; an arm --arms leaves out does not run.
    std::optional<int> parseRun(std::span<char*> arguments, RunArguments& parsed)
    {
        if (arguments.empty())
        {
            return usageError("expected a configuration id");
        }
        parsed.entry = find(arguments[0]);
        if (parsed.entry == nullptr)
        {
            return usageError(std::string("unknown configuration '") + arguments[0] + "'; `RojoRHIBench list` prints them");
        }
        std::optional<Mode> mode;
        std::string order;
        std::string arms;
        std::string dump;
        bool validation = false;
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            const std::string_view flag = arguments[index];
            if (flag == "--validation")
            {
                validation = true;
                continue;
            }
            if (index + 1 == arguments.size())
            {
                return usageError("missing a value after " + std::string(flag));
            }
            const std::string_view value = arguments[++index];
            if (flag == "--mode" && (value == "correctness" || value == "timing"))
            {
                mode = value == "timing" ? Mode::Timing : Mode::Correctness;
            }
            else if (flag == "--order" && (value == "RN" || value == "NR"))
            {
                order = value;
            }
            else if (flag == "--arms")
            {
                arms = value;
            }
            else if (flag == "--out")
            {
                parsed.out = value;
            }
            else if (flag == "--dump")
            {
                dump = value;
            }
            else
            {
                return usageError("unknown flag or value: " + std::string(flag) + " " + std::string(value));
            }
        }
        if (!mode || order.empty() || arms.empty() || parsed.out.empty())
        {
            return usageError("--mode, --order, --arms and --out are required");
        }
#if defined(NDEBUG)
        if (validation)
        {
            return usageError("--validation needs a Debug build: DeviceDesc::validation is refused in Release");
        }
#endif
        std::vector<Arm> requested;
        for (size_t start = 0; start <= arms.size();)
        {
            const size_t comma = std::min(arms.find(',', start), arms.size());
            const std::optional<Arm> arm = armFromName(std::string_view(arms).substr(start, comma - start));
            if (!arm)
            {
                return usageError("--arms takes R, N and F separated by commas, not '" + arms + "'");
            }
            if (!parsed.entry->config.hasArm(*arm))
            {
                return usageError(parsed.entry->config.id + " has no arm " + std::string(armName(*arm)));
            }
            requested.push_back(*arm);
            start = comma + 1;
        }
        const Arm first = order == "RN" ? Arm::Rojo : Arm::Native;
        const Arm second = order == "RN" ? Arm::Native : Arm::Rojo;
        for (const Arm arm : { first, second, Arm::Fixed })
        {
            if (std::ranges::find(requested, arm) != requested.end())
            {
                parsed.order.push_back(arm);
            }
        }
        parsed.options = runOptions(*mode);
        parsed.options.validation = validation;
        parsed.options.dumpDirectory = dump;
        return std::nullopt;
    }

    //==================================================================================================================
    // Runs each requested arm to completion, one after the other, then writes the dumps and the run JSON. A failed arm
    // is a result, recorded in the JSON; only a harness failure exits non-zero.
    int runRun(char* /*program*/, std::span<char*> arguments)
    {
        RunArguments parsed;
        if (const std::optional<int> error = parseRun(arguments, parsed))
        {
            return *error;
        }
        const Config& config = parsed.entry->config;
        const std::string thermalStart(thermalState());
        std::vector<ArmResult> results;
        results.reserve(parsed.order.size());
        for (const Arm arm : parsed.order)
        {
            results.push_back(parsed.entry->runner(arm)(config, parsed.options));
        }
        const std::string thermalEnd(thermalState());

        if (!parsed.options.dumpDirectory.empty())
        {
            for (const ArmResult& result : results)
            {
                if (const auto dumped = dumpOutputs(parsed.options.dumpDirectory, config.id, result.arm, result.outputs); !dumped)
                {
                    std::fprintf(stderr, "run: %s\n", dumped.error().c_str());
                    return 1;
                }
            }
        }
        const std::string device = deviceName();
        const std::string os = osVersion();
        const RunRecord record{
            .config = config,
            .mode = parsed.options.mode,
            .validation = parsed.options.validation,
            .device = device,
            .os = os,
            .thermalStart = thermalStart,
            .thermalEnd = thermalEnd,
            .arms = results,
        };
        const std::string text = runJson(record);
        std::ofstream file(std::filesystem::path(parsed.out), std::ios::binary);
        file << text << '\n';
        file.close();
        if (!file)
        {
            std::fprintf(stderr, "run: cannot write %s\n", parsed.out.c_str());
            return 1;
        }
        return 0;
    }

    //==================================================================================================================
    int runCheck(char* program, std::span<char*> arguments)
    {
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        argv.push_back(program);
        argv.insert(argv.end(), arguments.begin(), arguments.end());
        Catch::Session session;
        return session.run(static_cast<int>(argv.size()), argv.data());
    }

    constexpr Command kCommands[] = {
        { "list", "list", runList },
        { "environment", "environment OUT.json", runEnvironment },
        { "run", "run ID --mode correctness|timing --order RN|NR --arms R,N[,F] --out OUT.json [--dump DIR] [--validation]", runRun },
        { "check", "check [Catch2 arguments]", runCheck },
    };

    //==================================================================================================================
    int printUsage()
    {
        std::fprintf(stderr, "usage:\n");
        for (const Command& command : kCommands)
        {
            std::fprintf(stderr, "  RojoRHIBench %.*s\n", static_cast<int>(command.usage.size()), command.usage.data());
        }
        return kUsageError;
    }
} // namespace

//======================================================================================================================
int main(int argc, char** argv)
{
    // Installed before anything else allocates on another thread, so every counted frame sees the forwarders.
    const auto counter = installAllocationCounter();
    if (!counter)
    {
        std::fprintf(stderr, "%s\n", counter.error().c_str());
        return 1;
    }
    if (argc < 2)
    {
        return printUsage();
    }
    const std::string_view name = argv[1];
    for (const Command& command : kCommands)
    {
        if (command.name == name)
        {
            return command.run(argv[0], std::span<char*>(argv + 2, static_cast<size_t>(argc - 2)));
        }
    }
    std::fprintf(stderr, "unknown command '%s'\n", argv[1]);
    return printUsage();
}
