//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.h
/// @brief Declares the Null backend's device state.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Device.h>
#include <rojoRHI/Queue.h>
#include <rojoRHI/QueueType.h>

#include "Core/DeviceCore.h"

#include <array>
#include <cstdint>

namespace rojoRHI {
    /// The Null device's `Limits::maxAddrOffset`: the 32-bit byte offset an address carries in its `hi` word, 2^32 - 1.
    constexpr uint64_t kNullMaxAddrOffset = 4'294'967'295;

    struct DeviceImpl;

    /// The Null backend's queue: it knows its type and the device that owns it, which is how a queue used with another device
    /// is told apart. It executes nothing; a submission completes during the call.
    struct QueueImpl
    {
        DeviceImpl* device = nullptr;         ///< The device that owns the queue.
        QueueType type = QueueType::Graphics; ///< The queue's type.

        /// Returns the state `queue` wraps; checks VU-rojo-core-0004.
        static QueueImpl& of(Queue queue);

        /// Returns the queue handle that wraps `impl`.
        static Queue wrap(QueueImpl* impl);
    };

    /// The Null backend's device: the shared core, which also keeps the capabilities it reports. It renders nothing.
    struct DeviceImpl
    {
        /// Creates the state of a device from a description `core::validateDeviceDesc` accepted.
        explicit DeviceImpl(const DeviceDesc& desc) : core(desc)
        {
            for (size_t i = 0; i < queues.size(); ++i)
            {
                queues[i].device = this;
                queues[i].type = static_cast<QueueType>(i);
            }
        }

        core::DeviceCore core;                ///< Frame bookkeeping, pools, labels, capabilities and the format table.
        std::array<QueueImpl, 3> queues = {}; ///< One queue per `QueueType`, indexed by its value.

        /// Returns the state `device` wraps; checks VU-rojo-core-0004.
        static DeviceImpl& of(Device device);

        /// Returns the device handle that wraps `impl`.
        static Device wrap(DeviceImpl* impl);
    };
} // namespace rojoRHI
