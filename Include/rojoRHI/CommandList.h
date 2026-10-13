//----------------------------------------------------------------------------------------------------------------------
/// @file CommandList.h
/// @brief Declares the command list object handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {
    /// A recorded sequence of commands for one queue type. It wraps one backend pointer: copies refer to the same list, and a
    /// default-constructed one refers to none. It comes from a per-frame pool and returns to it when its frame retires; using
    /// it after that is undefined behaviour no tier detects. Its members arrive with the command recording chapter.
    class CommandList
    {
      public:
      private:
        struct CommandListImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
