//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TemporalScaler.h
/// @brief Owns MetalFX temporal reconstruction and its in-flight shared state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "Metal4Common.h"
#include "RHI/TemporalScaler.h"

#include <MetalFX/MetalFX.hpp>

#include <memory>
#include <string>

namespace lmx::rhi::metal4 {

// Every encoded frame retains this whole state until its slot retires. The wrapper may be
// destroyed or replaced while those frames still reference the scaler, fence and scratch output.
struct Metal4TemporalScalerState {
    ~Metal4TemporalScalerState();
    NS::SharedPtr<MTL4FX::TemporalScaler> scaler;
    NS::SharedPtr<MTL::Fence> fence;
    std::unique_ptr<Texture> privateOutput;
    TemporalScalerDesc desc;
    std::string label;
};

class Metal4TemporalScaler final : public TemporalScaler {
public:
    explicit Metal4TemporalScaler(std::shared_ptr<Metal4TemporalScalerState> state)
        : m_state(std::move(state)) {}
    const std::shared_ptr<Metal4TemporalScalerState>& state() const { return m_state; }

private:
    std::shared_ptr<Metal4TemporalScalerState> m_state;
};

} // namespace lmx::rhi::metal4
