//----------------------------------------------------------------------------------------------------------------------
/// @file Upscalers.cpp
/// @brief Implements the creation of an upscaler and the checks of the upscaling rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Upscalers.h"

#include "Base/Assert.h"
#include "Core/Formats.h"
#include "Core/Label.h"
#include "Core/Scopes.h"
#include "Core/Textures.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// A color format of the format tables: one a color attachment can use (chapter 15).
        [[maybe_unused]] bool isColorFormat(Format format)
        {
            return formatCeiling(format).colorAttachment;
        }

        //==============================================================================================================
        /// A two-channel floating-point format: `RG16Float` is the only one `Format` has.
        [[maybe_unused]] bool isMotionFormat(Format format)
        {
            return format == Format::RG16Float;
        }

        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(TextureUsage usage, TextureUsage flag)
        {
            return (usage & flag) != TextureUsage::None;
        }

        //==============================================================================================================
        [[maybe_unused]] bool sameExtent(const TextureRecord& texture, Extent2D extent)
        {
            return texture.extent.width == extent.width && texture.extent.height == extent.height;
        }

        //==============================================================================================================
        /// Names the kind for diagnostics.
        [[maybe_unused]] const char* kindName(UpscalerKind kind)
        {
            switch (kind)
            {
            case UpscalerKind::Auto: return "Auto";
            case UpscalerKind::MetalFX: return "MetalFX";
            case UpscalerKind::DLSS: return "DLSS";
            case UpscalerKind::FSR: return "FSR";
            case UpscalerKind::XeSS: return "XeSS";
            }
            return "unknown";
        }
    } // namespace

    //==================================================================================================================
    Result<Upscaler> createUpscaler(DeviceCore& core, const UpscalerDesc& desc)
    {
        ROJORHI_VU(
            desc.input.width > 0 && desc.input.height > 0, "VU-rojo-upscale-0001", "the input extent {}x{} has a zero axis", desc.input.width, desc.input.height
        );
        ROJORHI_VU(
            desc.output.width >= desc.input.width && desc.output.height >= desc.input.height,
            "VU-rojo-upscale-0002",
            "the output extent {}x{} is smaller than the input extent {}x{} in an axis",
            desc.output.width,
            desc.output.height,
            desc.input.width,
            desc.input.height
        );
        ROJORHI_VU(
            isColorFormat(desc.color) && isColorFormat(desc.outputFormat) && isDepthFormat(desc.depth) && isMotionFormat(desc.motion),
            "VU-rojo-upscale-0003",
            "the formats do not fit their roles: color {} and outputFormat {} are color formats, depth {} is a depth format and motion {} is a two-channel "
            "floating-point format",
            std::to_underlying(desc.color),
            std::to_underlying(desc.outputFormat),
            std::to_underlying(desc.depth),
            std::to_underlying(desc.motion)
        );
        UpscalerRecord record;
        record.label = resolveLabel(desc.label, "rojorhi.upscaler.unnamed");
        const std::span<const UpscalerKind> offered = core.capabilities().upscalers;
        if (desc.kind == UpscalerKind::Auto)
        {
            if (offered.empty())
            {
                return std::unexpected(Error{ ErrorCategory::Unsupported, "the device lists no upscaler kind for Auto to select" });
            }
            record.kind = offered.front();
        }
        else if (std::find(offered.begin(), offered.end(), desc.kind) != offered.end())
        {
            record.kind = desc.kind;
        }
        else
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("the device cannot create a {} upscaler", kindName(desc.kind)) });
        }
        record.input = desc.input;
        record.output = desc.output;
        record.color = desc.color;
        record.depth = desc.depth;
        record.motion = desc.motion;
        record.outputFormat = desc.outputFormat;
        record.flags = desc.flags;
        return core.pool<Upscaler>().allocate(std::move(record));
    }

    //==================================================================================================================
    void upscale(CommandListCore& list, [[maybe_unused]] Upscaler upscaler, const UpscaleParams& params)
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(list.type == QueueType::Graphics, "VU-rojo-upscale-0006", "upscale was recorded on the non-Graphics list \"{}\"", list.label);
        ROJORHI_VU(!list.passOpen, "VU-rojo-upscale-0007", "upscale was recorded on the list \"{}\" with a render pass open", list.label);
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0020).
        UpscalerRecord* scaler = list.device->pool<Upscaler>().checked(upscaler, false);
        auto& textures = list.device->pool<Texture>();
        const TextureRecord* color = textures.checked(params.color, true);
        const TextureRecord* depth = textures.checked(params.depth, true);
        const TextureRecord* motion = textures.checked(params.motion, true);
        const TextureRecord* output = textures.checked(params.output, true);
        const TextureRecord* exposure = textures.checked(params.exposure, true);
        const TextureRecord* reactive = textures.checked(params.reactive, true);
        for (const auto& [record, use] : { std::pair{ color, "upscale color" },
                                           std::pair{ depth, "upscale depth" },
                                           std::pair{ motion, "upscale motion" },
                                           std::pair{ output, "upscale output" },
                                           std::pair{ exposure, "upscale exposure" },
                                           std::pair{ reactive, "upscale reactive" } })
        {
            if (record != nullptr)
            {
                checkNotSwapchainImage(*record, use);
            }
        }
        ROJORHI_VU(params.preExposure > 0.0f, "VU-rojo-upscale-0004", "preExposure {} is not greater than zero", params.preExposure);
        ROJORHI_VU(params.sharpness >= 0.0f && params.sharpness <= 1.0f, "VU-rojo-upscale-0005", "sharpness {} is outside [0, 1]", params.sharpness);
        ROJORHI_VU(
            color != nullptr && depth != nullptr && motion != nullptr && output != nullptr,
            "VU-rojo-upscale-0009",
            "upscale needs color, depth, motion and output; a null handle was passed for one of them"
        );
        ROJORHI_VU(
            color->format == scaler->color && depth->format == scaler->depth && motion->format == scaler->motion && sameExtent(*color, scaler->input) &&
                sameExtent(*depth, scaler->input) && sameExtent(*motion, scaler->input),
            "VU-rojo-upscale-0010",
            "the color, depth or motion texture does not have the format of the upscaler \"{}\" and an extent of {}x{}",
            scaler->label,
            scaler->input.width,
            scaler->input.height
        );
        ROJORHI_VU(
            output->format == scaler->outputFormat && sameExtent(*output, scaler->output) && params.output != params.color && params.output != params.depth &&
                params.output != params.motion && params.output != params.exposure && params.output != params.reactive,
            "VU-rojo-upscale-0011",
            "the output texture \"{}\" does not have the format and the {}x{} extent of the upscaler \"{}\" or is also an input",
            output->label,
            scaler->output.width,
            scaler->output.height,
            scaler->label
        );
        ROJORHI_VU(
            (exposure == nullptr || (exposure->format == Format::R16Float && exposure->extent.width == 1 && exposure->extent.height == 1)) &&
                (reactive == nullptr || (reactive->format == Format::R8Unorm && sameExtent(*reactive, scaler->input))),
            "VU-rojo-upscale-0012",
            "the exposure texture is not 1x1 R16Float or the reactive texture is not R8Unorm of {}x{}",
            scaler->input.width,
            scaler->input.height
        );
        ROJORHI_VU(
            hasUsage(color->usage, TextureUsage::Sampled) && hasUsage(depth->usage, TextureUsage::Sampled) && hasUsage(motion->usage, TextureUsage::Sampled) &&
                (exposure == nullptr || hasUsage(exposure->usage, TextureUsage::Sampled)) &&
                (reactive == nullptr || hasUsage(reactive->usage, TextureUsage::Sampled)),
            "VU-rojo-upscale-0013",
            "an input of upscale was not created with TextureUsage::Sampled"
        );
        ROJORHI_VU(
            hasUsage(output->usage, TextureUsage::Storage) && hasUsage(output->usage, TextureUsage::RenderTarget),
            "VU-rojo-upscale-0014",
            "the output texture \"{}\" was not created with both TextureUsage::Storage and TextureUsage::RenderTarget",
            output->label
        );
        const bool wholeInput = params.inputContent.width == 0 && params.inputContent.height == 0;
        ROJORHI_VU(
            wholeInput || (params.inputContent.width >= 1 && params.inputContent.width <= scaler->input.width && params.inputContent.height >= 1 &&
                           params.inputContent.height <= scaler->input.height),
            "VU-rojo-upscale-0015",
            "inputContent {}x{} is neither zero in both axes nor inside the input extent {}x{}",
            params.inputContent.width,
            params.inputContent.height,
            scaler->input.width,
            scaler->input.height
        );
        // The upscaler's last frame is exchanged without a lock, after the other checks so a rejected upscale does not count; a second upscale in one frame
        // reads the same frame back.
        const uint64_t previous = scaler->lastFrame.frame.exchange(list.frame, std::memory_order_relaxed);
        ROJORHI_VU(previous != list.frame, "VU-rojo-upscale-0008", "the upscaler \"{}\" was already used by an upscale in frame {}", scaler->label, list.frame);
#endif
        // The operation forms its own debug group, a scope of the timings report, named `params.label`.
        recordScope(list, params.label, "rojorhi.upscale.unnamed", static_cast<uint32_t>(list.scopes.size()));
    }
} // namespace rojoRHI::core
