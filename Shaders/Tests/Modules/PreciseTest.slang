//----------------------------------------------------------------------------------------------------------------------
/// @file PreciseTest.slang
/// @brief Shares the dynamic precision probe's flat root with its entry and the cross-target layout probe.
//----------------------------------------------------------------------------------------------------------------------

module PreciseTest;

import RojoRHI;

namespace rojoRHI::test
{
    public struct PreciseRoot
    {
        public rojoRHI::Addr input;
        public rojoRHI::Addr output;
    };
}
