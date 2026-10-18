//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommitTiming.cpp
/// @brief Implements the private seam that makes a queue's commits report their GPU time.
//----------------------------------------------------------------------------------------------------------------------

#include "CommitTiming.h"

#include <rojoRHI/RHI.h>

#include "Metal4Device.h"

namespace rojoRHI::metal4 {
    //==================================================================================================================
    void setCommitTiming(Queue queue, CommitTimingCallback callback, void* user)
    {
        QueueImpl& impl = QueueImpl::of(queue);
        impl.commitCallback = callback;
        impl.commitUser = user;
        impl.commitSerial = 0;
    }
} // namespace rojoRHI::metal4
