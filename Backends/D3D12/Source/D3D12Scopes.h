//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Scopes.h
/// @brief Declares the native half of a D3D12 scope, shared by the scope entries and the render passes that form scopes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {
    struct CommandListImpl;
} // namespace rojoRHI

namespace rojoRHI::d3d12 {
    /// Records the native begin of the scope the core recorded last on `list`: a PIX event with its label, widened into the
    /// list's scratch, and the timestamp query that begins the pair it claims, whose first index it pushes on the list's open
    /// pairs and encodes in the record. An untimed scope pushes `core::kNoTimestamp` and records the event alone. Each native
    /// call counts once.
    void beginNativeScope(CommandListImpl& list);

    /// Records the native end of the innermost scope open on `list`: the timestamp query that ends its pair, when it is timed,
    /// then the end of its PIX event. Pops the pair; each native call counts once.
    void endNativeScope(CommandListImpl& list);
} // namespace rojoRHI::d3d12
