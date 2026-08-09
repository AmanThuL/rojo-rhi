#pragma once

#include "RHI/Metal4/Metal4Common.h"
#include "RHI/RHI.h"

#include <string>
#include <string_view>
#include <utility>

namespace lmx::rhi::metal4::device_detail {

// NS::String::utf8String() may return null; converting it directly to std::string is undefined.
inline std::string toStdString(const NS::String* text, std::string fallback = {}) {
    if (text == nullptr) {
        return fallback;
    }
    const char* utf8 = text->utf8String();
    return utf8 != nullptr ? std::string(utf8) : fallback;
}

inline std::string describe(NS::Error* error) {
    if (error == nullptr) {
        return "no additional detail";
    }
    return toStdString(error->localizedDescription(), "no additional detail");
}

inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

inline std::string_view resolveLabel(std::string_view label, std::string_view fallback) {
    return label.empty() ? fallback : label;
}

inline NS::SharedPtr<NS::String> labelOrFallback(std::string_view label,
                                                 std::string_view fallback) {
    return makeString(resolveLabel(label, fallback));
}

} // namespace lmx::rhi::metal4::device_detail
