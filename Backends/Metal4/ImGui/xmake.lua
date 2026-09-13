-- Optional Dear ImGui renderer integration. Keeping this in a separate target lets the core RHI
-- build and link without ImGui while App opts into the editor-specific bridge explicitly.
target("RHIMetal4ImGui")
    set_kind("static")
    add_files("Source/Metal4ImGui.cpp")
    add_files("Source/ImGuiBackendContract.cpp")
    add_includedirs("Include", {public = true})
    add_includedirs("../Source")
    add_includedirs("../../../../ThirdParty/metal-cpp")
    add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
    add_deps("Core", "RHI", "ImGui")
