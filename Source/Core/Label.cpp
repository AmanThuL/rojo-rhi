//----------------------------------------------------------------------------------------------------------------------
/// @file Label.cpp
/// @brief Implements the label check and default.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Label.h"

#include "Base/Assert.h"

#include <cstdint>

namespace rojoRHI::core {
    //==================================================================================================================
    bool isValidUtf8(std::string_view text)
    {
        size_t i = 0;
        while (i < text.size())
        {
            const uint8_t lead = static_cast<uint8_t>(text[i]);
            size_t extra = 0;
            uint32_t minimum = 0;
            uint32_t code = 0;
            if (lead < 0x80)
            {
                ++i;
                continue;
            }
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                extra = 1;
                minimum = 0x80;
                code = lead & 0x1Fu;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                extra = 2;
                minimum = 0x800;
                code = lead & 0x0Fu;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                extra = 3;
                minimum = 0x10000;
                code = lead & 0x07u;
            }
            else
            {
                return false;
            }
            if (i + extra >= text.size())
            {
                return false;
            }
            for (size_t k = 1; k <= extra; ++k)
            {
                const uint8_t next = static_cast<uint8_t>(text[i + k]);
                if ((next & 0xC0u) != 0x80u)
                {
                    return false;
                }
                code = (code << 6) | (next & 0x3Fu);
            }
            if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    //==================================================================================================================
    void checkLabel([[maybe_unused]] std::string_view label)
    {
        ROJORHI_VU(label.find('\0') == std::string_view::npos, "VU-rojo-core-0007", "a label contains a NUL character");
        ROJORHI_VU(isValidUtf8(label), "VU-rojo-core-0006", "a label is not valid UTF-8");
    }

    //==================================================================================================================
    std::string resolveLabel(std::string_view label, std::string_view defaultLabel)
    {
        checkLabel(label);
        return std::string(label.empty() ? defaultLabel : label);
    }
} // namespace rojoRHI::core
