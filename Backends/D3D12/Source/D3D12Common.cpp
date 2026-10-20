//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.cpp
/// @brief Implements the helpers every D3D12 backend file shares: the refusal and the abort of an entry the backend does not implement.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include "Base/Log.h"

#include <cstdlib>
#include <format>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    void notImplemented(std::string_view entry)
    {
        ROJORHI_LOG_ERROR("{} is not implemented by the D3D12 backend", entry);
        std::abort();
    }

    //==================================================================================================================
    std::unexpected<Error> unsupported(std::string_view entry)
    {
        return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("{} is not supported by the D3D12 backend", entry) });
    }
} // namespace rojoRHI::d3d12
