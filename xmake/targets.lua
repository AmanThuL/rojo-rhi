-- Every target of the RHI component, includable from this component's own root and from a host
-- project that mounts the component. The file is the component's build interface: a host includes
-- it and nothing else, so project settings, language level and package requires stay declared once
-- per root rather than once per include.
--
-- The shader rule the test target opts into is part of that interface, so this file includes it
-- rather than leaving a host to remember a second include.
includes("shaders.lua")

-- Two values a host may set as plain globals before the include, because xmake evaluates an
-- included description script in the same interpreter as the including one:
--
--   rhi_thirdparty    directory holding metal-cpp; defaults to <component>/ThirdParty
--   rhi_imgui_target  name of the host's Dear ImGui target; absent means no ImGui adapter
--
-- A configure option would be the obvious home for the first, but get_config() still reads nil on
-- the description pass that resolves options, so an option cannot feed add_includedirs without the
-- include directory differing between passes. A global is read the same way on every pass.
--
-- xmake resolves add_files and add_includedirs against the directory of the script that declares
-- them, so every path inside the component is written relative to this file and reads the same
-- under either root. A host's third-party directory is outside the component and arrives absolute,
-- so it is made relative to this script for the same reason.
--
-- The shader rule runs in script scope, where this script's directory is no longer the anchor, so
-- the two values it reads off the target are absolute instead.
local component = ".."
local component_dir = path.directory(os.scriptdir())
local thirdparty_dir = rhi_thirdparty and path.absolute(rhi_thirdparty)
                       or path.join(component_dir, "ThirdParty")
local thirdparty = path.relative(thirdparty_dir, os.scriptdir())
local imgui_target = rhi_imgui_target

-- Backend-neutral RHI surface and the Metal 4 implementation. The public include directory
-- preserves the existing #include "RHI/..." contract; metal-cpp and backend headers remain
-- implementation details of this component.
target("RHI")
    set_kind("static")
    add_files(path.join(component, "Source/*.cpp"))
    add_files(path.join(component, "Source/Base/*.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4Capture.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4CommandList.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4Device.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4DeviceFrame.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4DevicePipeline.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4DeviceResources.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4FrameArena.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4Resources.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4Swapchain.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/Metal4TemporalScaler.cpp"))
    add_files(path.join(component, "Backends/Metal4/Source/MetalCppImpl.cpp"))
    add_includedirs(path.join(component, "Include"), {public = true})
    add_includedirs(path.join(component, "Source"))
    add_includedirs(path.join(component, "Backends/Metal4/Source"))
    add_includedirs(path.join(thirdparty, "metal-cpp"))
    add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")

-- Optional Dear ImGui renderer integration. Keeping this in a separate target lets the core RHI
-- build and link without ImGui while a host opts into the editor-specific bridge explicitly. Only
-- a host that owns an ImGui target can supply the headers this adapter compiles against, so a
-- standalone configure of the component simply has no such target.
if imgui_target then
    target("RHIMetal4ImGui")
        set_kind("static")
        add_files(path.join(component, "Backends/Metal4/ImGui/Source/Metal4ImGui.cpp"))
        add_files(path.join(component, "Backends/Metal4/ImGui/Source/ImGuiBackendContract.cpp"))
        add_includedirs(path.join(component, "Backends/Metal4/ImGui/Include"), {public = true})
        add_includedirs(path.join(component, "Backends/Metal4/Source"))
        add_includedirs(path.join(component, "Source"))
        add_includedirs(path.join(thirdparty, "metal-cpp"))
        add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
        add_deps("RHI", imgui_target)
end

-- Contract and GPU tests for the standalone RHI component. They link the RHI target alone, so
-- the suite stays runnable once the component leaves this repository.
target("RHITests")
    set_kind("binary")
    set_default(false)
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rhi-test")
    add_files(path.join(component, "Tests/*.cpp"))
    add_deps("RHI")
    add_packages("catch2", "glm")
    -- GPU cases load shaders relative to the test binary, so this target compiles its own smoke
    -- shaders out of the component's own tree. Six of them are byte-identical copies of oracles a
    -- host's own suite may also own: duplicated deliberately, so nothing here reaches outside the
    -- component. The non-recursive pattern keeps Modules/ off the entry-point list.
    add_rules("rhi_slang2metallib")
    set_values("slang.moduledir", path.join(component_dir, "Shaders/Tests/Modules"))
    set_values("slang.slangc", path.join(thirdparty_dir, "slang/bin/slangc"))
    add_files(path.join(component, "Shaders/Tests/*.slang"))
    add_tests("unit", {runargs = {"~[gpu]"}})
    -- Hidden diagnostics have explicit reproduction commands and are not ordinary regression gates.
    add_tests("gpu", {runargs = {"[gpu]~[.]"}})
