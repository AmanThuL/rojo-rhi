//----------------------------------------------------------------------------------------------------------------------
/// @file Config.cpp
/// @brief Implements arm names and the JSON forms of a configuration.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Config.h"

#include "Common/JsonWriter.h"

#include <algorithm>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::string_view armName(Arm arm)
    {
        switch (arm)
        {
        case Arm::Rojo: return "R";
        case Arm::Native: return "N";
        case Arm::Fixed: return "F";
        }
        return "?";
    }

    //==================================================================================================================
    std::optional<Arm> armFromName(std::string_view name)
    {
        for (const Arm arm : { Arm::Rojo, Arm::Native, Arm::Fixed })
        {
            if (armName(arm) == name)
            {
                return arm;
            }
        }
        return std::nullopt;
    }

    //==================================================================================================================
    bool Config::hasArm(Arm arm) const
    {
        return std::ranges::find(arms, arm) != arms.end();
    }

    //==================================================================================================================
    void writeParams(JsonWriter& json, const Config& config)
    {
        json.beginObject();
        if (config.k)
        {
            json.key("K").unsignedInteger(*config.k);
        }
        if (config.maxCount)
        {
            json.key("maxCount").unsignedInteger(*config.maxCount);
        }
        if (config.nullCount)
        {
            json.key("nullCount").boolean(*config.nullCount);
        }
        if (config.pattern)
        {
            json.key("pattern").string(*config.pattern);
        }
        json.endObject();
    }

    //==================================================================================================================
    std::string listJson(const Config& config)
    {
        JsonWriter json;
        json.beginObject();
        json.key("id").string(config.id);
        json.key("workload").string(config.workload);
        json.key("params");
        writeParams(json, config);
        json.key("arms").beginArray();
        for (const Arm arm : config.arms)
        {
            json.string(armName(arm));
        }
        json.endArray();
        json.endObject();
        return json.text();
    }
} // namespace rojoRHI::bench
