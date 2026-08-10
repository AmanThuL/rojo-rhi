//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiBackendContract.cpp
/// @brief Verifies the pinned Dear ImGui Metal 4 backend contract at compile time.
//----------------------------------------------------------------------------------------------------------------------
#include <imgui_impl_metal4.h>

#include <type_traits>

static_assert(std::is_same_v<decltype(&ImGui_ImplMetal4_RemoveTexture), bool (*)(MTL::Texture*)>);
