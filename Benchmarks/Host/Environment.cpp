//----------------------------------------------------------------------------------------------------------------------
/// @file Environment.cpp
/// @brief Implements the environment writer over the system Metal device and NSProcessInfo.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/Environment.h"

#include "Common/JsonWriter.h"
#include "Host/ThermalState.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        constexpr std::array<std::pair<MTL::GPUFamily, std::string_view>, 5> kFamilies{ {
            { MTL::GPUFamilyApple7, "Apple7" },
            { MTL::GPUFamilyApple8, "Apple8" },
            { MTL::GPUFamilyApple9, "Apple9" },
            { MTL::GPUFamilyApple10, "Apple10" },
            { MTL::GPUFamilyMetal4, "Metal4" },
        } };
    } // namespace

    //==================================================================================================================
    std::expected<void, std::string> writeEnvironmentFile(const std::filesystem::path& path)
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        MTL::Device* device = MTL::CreateSystemDefaultDevice();
        const std::string os = NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String();

        JsonWriter json;
        json.beginObject();
        json.key("device");
        if (device != nullptr)
        {
            json.string(device->name()->utf8String());
        }
        else
        {
            json.null();
        }
        json.key("families").beginArray();
        for (const auto& [family, name] : kFamilies)
        {
            if (device != nullptr && device->supportsFamily(family))
            {
                json.string(name);
            }
        }
        json.endArray();
        json.key("metal4").boolean(device != nullptr && device->supportsFamily(MTL::GPUFamilyMetal4));
        json.key("timestampHz");
        if (device != nullptr)
        {
            json.unsignedInteger(device->queryTimestampFrequency());
        }
        else
        {
            json.null();
        }
        json.key("os").string(os);
        json.key("thermal").string(thermalState());
        json.endObject();

        const bool hasDevice = device != nullptr;
        if (device != nullptr)
        {
            device->release();
        }
        pool->release();

        auto saved = json.save(path);
        if (!saved)
        {
            return saved;
        }
        if (!hasDevice)
        {
            return std::unexpected(std::string("environment: MTLCreateSystemDefaultDevice returned no device"));
        }
        return {};
    }

    //==================================================================================================================
    std::string deviceName()
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        MTL::Device* device = MTL::CreateSystemDefaultDevice();
        std::string name = device != nullptr ? device->name()->utf8String() : "none";
        if (device != nullptr)
        {
            device->release();
        }
        pool->release();
        return name;
    }

    //==================================================================================================================
    std::string osVersion()
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        std::string version = NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String();
        pool->release();
        return version;
    }
} // namespace rojoRHI::bench
