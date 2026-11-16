//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12NoDialog.cpp
/// @brief Makes an abort or a CRT report in the D3D12 test process end it with a message on stderr, never a modal dialog.
//----------------------------------------------------------------------------------------------------------------------

#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>

#include <initializer_list>

namespace {
    //==================================================================================================================
    /// Routes the CRT's warnings, errors and asserts to stderr, keeps abort from writing its dialog or reporting a fault, and
    /// turns off the system's critical-error and fault boxes, as a death test's child does, so a failure outside a death
    /// test ends the run at once instead of holding the step until its timeout. Returns true.
    bool suppressDialogs()
    {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        for ([[maybe_unused]] const int kind : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
        {
            _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
        }
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        return true;
    }

    /// Applies `suppressDialogs` while the test executable starts, before Catch2 runs any case.
    [[maybe_unused]] const bool kDialogsSuppressed = suppressDialogs();
} // namespace
