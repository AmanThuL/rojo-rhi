-- Backend-neutral RHI surface and the Metal 4 implementation. The public include directory
-- preserves the existing #include "RHI/..." contract; metal-cpp and backend headers remain
-- implementation details of this component.
target("RHI")
    set_kind("static")
    add_files("Source/*.cpp")
    add_files("Backends/Metal4/Source/Metal4Capture.cpp")
    add_files("Backends/Metal4/Source/Metal4CommandList.cpp")
    add_files("Backends/Metal4/Source/Metal4Device.cpp")
    add_files("Backends/Metal4/Source/Metal4DeviceFrame.cpp")
    add_files("Backends/Metal4/Source/Metal4DevicePipeline.cpp")
    add_files("Backends/Metal4/Source/Metal4DeviceResources.cpp")
    add_files("Backends/Metal4/Source/Metal4FrameArena.cpp")
    add_files("Backends/Metal4/Source/Metal4Resources.cpp")
    add_files("Backends/Metal4/Source/Metal4Swapchain.cpp")
    add_files("Backends/Metal4/Source/Metal4TemporalScaler.cpp")
    add_files("Backends/Metal4/Source/MetalCppImpl.cpp")
    add_includedirs("Include", {public = true})
    add_includedirs("Backends/Metal4/Source")
    add_includedirs("../ThirdParty/metal-cpp")
    add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
    add_deps("Core")

-- Optional Dear ImGui renderer integration. Keeping this in a separate target lets the core RHI
-- build and link without ImGui while App opts into the editor-specific bridge explicitly.
target("RHIMetal4ImGui")
    set_kind("static")
    add_files("Backends/Metal4/Source/Metal4ImGui.cpp")
    add_files("Backends/Metal4/Source/ImGuiBackendContract.cpp")
    add_includedirs("Backends/Metal4/ImGui/Include", {public = true})
    add_includedirs("Backends/Metal4/Source")
    add_includedirs("../ThirdParty/metal-cpp")
    add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
    add_deps("Core", "RHI", "ImGui")
