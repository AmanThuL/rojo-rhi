//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capture.h
/// @brief Declares optional Metal 4 GPU capture controls.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Device.h"

#include <string_view>

/// Developer tooling, not part of the RHI surface: a Metal 4 GPU capture written as a .gputrace
/// document that Xcode can open. Deliberately metal-cpp-free so the App target -- which does not
/// have the metal-cpp include directory -- can call it.

namespace lmx::rhi::metal4 {

/// Begins a GPU capture and writes the resulting document to `outPath`.
bool beginCapture(Device& device, std::string_view outPath);

/// Closes the capture beginCapture() opened and logs the document that was written. No-op when no
/// capture is running. The caller is responsible for the GPU having finished the work it wants in
/// the document (Device::waitIdle) before calling this.
void endCapture();

} // namespace lmx::rhi::metal4
