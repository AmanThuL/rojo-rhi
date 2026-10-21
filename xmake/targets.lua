-- Every target of the RHI component, includable from this component's own root and from a host
-- project that mounts the component. The file is the component's build interface: a host includes
-- it and nothing else, so project settings, language level and package requires stay declared once
-- per root rather than once per include.
--
-- The shader rules the test targets opt into, and the rule that compiles the Metal 4 backend's own
-- kernels, are part of that interface, so this file includes them rather than leaving a host to
-- remember more includes.
includes("shaders.lua", "slang.lua", "metal.lua", "hlsl.lua", "kernels.lua")

-- Two values a host may set as plain globals before the include, because xmake evaluates an
-- included description script in the same interpreter as the including one:
--
--   rojorhi_thirdparty    directory holding metal-cpp; defaults to <component>/ThirdParty
--   rojorhi_imgui_target  name of the host's Dear ImGui target; absent means no ImGui adapter
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
local thirdparty_dir = rojorhi_thirdparty and path.absolute(rojorhi_thirdparty)
                       or path.join(component_dir, "ThirdParty")
local thirdparty = path.relative(thirdparty_dir, os.scriptdir())
local imgui_target = rojorhi_imgui_target

-- The frozen v1 interface lives under Legacy/ and builds on macOS only, because its one backend is
-- Metal 4. Its targets carry the Legacy marker; the consumer's dependency names them until the
-- consumer port deletes Legacy/ and these three blocks (ADR 0017).
if is_plat("macosx") then
    -- Frozen v1 interface: backend-neutral RHI surface and the Metal 4 implementation. The public
    -- include directory preserves the existing #include <rojoRHI/...> contract; metal-cpp and
    -- backend headers remain implementation details of this component.
    target("RojoRHILegacy")
        set_kind("static")
        add_files(path.join(component, "Legacy/Source/*.cpp"))
        add_files(path.join(component, "Legacy/Source/Base/*.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4Capture.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4CommandList.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4Device.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4DeviceFrame.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4DevicePipeline.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4DeviceResources.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4FrameArena.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4Resources.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4Swapchain.cpp"))
        add_files(path.join(component, "Legacy/Backends/Metal4/Source/Metal4TemporalScaler.cpp"))
        add_includedirs(path.join(component, "Legacy/Include"), {public = true})
        add_includedirs(path.join(component, "Legacy/Source"))
        add_includedirs(path.join(component, "Legacy/Backends/Metal4/Source"))
        add_includedirs(path.join(thirdparty, "metal-cpp"))
        add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
        add_deps("RojoRHIMetalCpp")

    -- Frozen v1 interface: optional Dear ImGui renderer integration. Keeping this in a separate
    -- target lets the core RHI build and link without ImGui while a host opts into the
    -- editor-specific bridge explicitly. Only a host that owns an ImGui target can supply the
    -- headers this adapter compiles against, so a standalone configure of the component simply has
    -- no such target.
    if imgui_target then
        target("RojoRHILegacyMetal4ImGui")
            set_kind("static")
            add_files(path.join(component, "Legacy/Backends/Metal4/ImGui/Source/Metal4ImGui.cpp"))
            add_files(path.join(component, "Legacy/Backends/Metal4/ImGui/Source/ImGuiBackendContract.cpp"))
            add_includedirs(path.join(component, "Legacy/Backends/Metal4/ImGui/Include"), {public = true})
            add_includedirs(path.join(component, "Legacy/Backends/Metal4/Source"))
            add_includedirs(path.join(component, "Legacy/Source"))
            add_includedirs(path.join(thirdparty, "metal-cpp"))
            add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation")
            add_deps("RojoRHILegacy", imgui_target)
    end

    -- Frozen v1 interface: contract and GPU tests for the standalone RHI component. They link the
    -- RojoRHILegacy target alone, so the suite stays runnable once the component leaves this
    -- repository.
    target("RojoRHILegacyTests")
        set_kind("binary")
        set_default(false)
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-legacy-test")
        add_files(path.join(component, "Legacy/Tests/*.cpp"))
        add_deps("RojoRHILegacy")
        add_packages("catch2", "glm")
        -- GPU cases load shaders relative to the test binary, so this target compiles its own smoke
        -- shaders out of the component's own tree. Six of them are byte-identical copies of oracles a
        -- host's own suite may also own: duplicated deliberately, so nothing here reaches outside the
        -- component. The non-recursive pattern keeps Modules/ off the entry-point list.
        add_rules("rojorhi_slang2metallib")
        set_values("slang.moduledir", path.join(component_dir, "Legacy/Shaders/Tests/Modules"))
        set_values("slang.slangc", path.join(thirdparty_dir, "slang/bin/slangc"))
        add_files(path.join(component, "Legacy/Shaders/Tests/*.slang"))
        add_tests("unit", {runargs = {"~[gpu]"}})
        -- Hidden diagnostics have explicit reproduction commands and are not ordinary regression gates.
        add_tests("gpu", {runargs = {"[gpu]~[.]"}})
end

-- The backend compiled into RojoRHI (ADR 0012); one backend per build.
-- The default is metal4 on a macOS host and null elsewhere. Option scope runs before the platform is
-- resolved, so is_plat answers nothing there; is_host does.
option("rojorhi_backend")
    if is_host("macosx") then
        set_default("metal4")
    else
        set_default("null")
    end
    set_values("null", "metal4", "d3d12")
    set_showmenu(true)
    set_description("Backend for RojoRHI: metal4 (builds on macOS only, the default there), d3d12 (builds on Windows only) or null (the GPU-free test backend, the default elsewhere; pass it on a Mac host for another platform)")
option_end()

-- The adapter a d3d12 build creates its device on, and the D3D12 debug features it enables. Both are
-- build configuration, compiled into the backend as defines: the library reads no environment variable.
option("rojorhi_d3d12_adapter")
    set_default("hardware")
    set_values("hardware", "warp")
    set_showmenu(true)
    set_description("Adapter a d3d12 build creates its device on: hardware (the default) or warp (the software rasterizer, for hosted CI)")
option_end()

option("rojorhi_d3d12_debug")
    set_default("mode")
    set_values("mode", "off", "layer", "gbv")
    set_showmenu(true)
    set_description("D3D12 debug features of a d3d12 build: mode (the debug layer and DRED in Debug builds, nothing in Release; the default), off, layer (the debug layer and DRED) or gbv (the debug layer, GPU-based validation and DRED)")
option_end()

-- Release builds use link-time optimization, so a call into the library inlines into its caller
-- (ADR 0012): ThinLTO with clang, /GL with /LTCG on MSVC. A host compiles its own Release targets
-- with the same policy for the call to inline into its code; its linker accepts the library's
-- objects either way.
local function release_lto()
    if is_mode("release") then
        set_policy("build.optimization.lto", true)
        if is_plat("windows") then
            add_arflags("/LTCG")
        end
    end
end

-- MSVC's C4702 (unreachable code) fails hosted Windows CI under /WX, and only an MSVC build sees it. clang's
-- -Wunreachable-code finds the same pattern, a FAIL or a return followed by more code, on a Mac. It is a warning here,
-- as every warning is off Windows. The v2 targets take it; v1 under Legacy/ is frozen (ADR 0017) and does not.
local function warn_unreachable()
    add_cxxflags("-Wunreachable-code", "-Wunreachable-code-return", {tools = "clang"})
end

-- An executable target calls this to turn off warnings C4702 (unreachable code) and C4722 (a destructor that never returns)
-- on a d3d12 Release build, where link-time code generation raised them across the library's aborting entries and /WX made
-- them errors; every other warning still fails it.
local function unreachable_after_stubs()
    if is_config("rojorhi_backend", "d3d12") and is_mode("release") then
        add_cxflags("/wd4702", "/wd4722")
    end
end

-- The adapter and debug features a d3d12 build selects, as private defines of the calling target: the
-- library acts on them, and the D3D12 cases check the device against them.
local function d3d12_selection()
    add_options("rojorhi_d3d12_adapter", "rojorhi_d3d12_debug")
    if is_config("rojorhi_d3d12_adapter", "warp") then
        add_defines("ROJORHI_D3D12_ADAPTER_WARP")
    else
        add_defines("ROJORHI_D3D12_ADAPTER_HARDWARE")
    end
    if is_config("rojorhi_d3d12_debug", "off") then
        add_defines("ROJORHI_D3D12_DEBUG_OFF")
    elseif is_config("rojorhi_d3d12_debug", "layer") then
        add_defines("ROJORHI_D3D12_DEBUG_LAYER")
    elseif is_config("rojorhi_d3d12_debug", "gbv") then
        add_defines("ROJORHI_D3D12_DEBUG_GBV")
    else
        add_defines("ROJORHI_D3D12_DEBUG_MODE")
    end
end

-- The D3D12 runtime an executable loads from its own directory, copied there after each build:
-- the Agility SDK's D3D12Core.dll and d3d12SDKLayers.dll into D3D12/, and WARP's d3d10warp.dll
-- beside the executable. The FidelityFX loader and upscaler DLLs are copied only when the
-- third-party directory holds them: the backend lists FSR only when its loader loads, so their
-- absence is a supported state. An executable adds the rule and sets the value rojorhi.thirdparty
-- to the absolute third-party directory setup fills; a host does the same for its own executable.
rule("rojorhi.d3d12_runtime")
    after_build(function (target)
        local thirdparty = target:values("rojorhi.thirdparty")
        if type(thirdparty) == "table" then
            thirdparty = thirdparty[1]
        end
        assert(thirdparty, format("%s adds rojorhi.d3d12_runtime without setting rojorhi.thirdparty", target:name()))
        local function copy(source, destination_dir, required)
            if not os.isfile(source) then
                if required then
                    raise("%s is missing; run xmake setup", source)
                end
                return
            end
            -- Compared by content, not time, so a pin moved to an older version is copied too.
            os.mkdir(destination_dir)
            os.cp(source, path.join(destination_dir, path.filename(source)), {copy_if_different = true})
        end
        local targetdir = target:targetdir()
        copy(path.join(thirdparty, "agility", "bin", "D3D12Core.dll"), path.join(targetdir, "D3D12"), true)
        copy(path.join(thirdparty, "agility", "bin", "d3d12SDKLayers.dll"), path.join(targetdir, "D3D12"), true)
        copy(path.join(thirdparty, "warp", "bin", "d3d10warp.dll"), targetdir, true)
        copy(path.join(thirdparty, "fidelityfx", "bin", "amd_fidelityfx_loader_dx12.dll"), targetdir, false)
        copy(path.join(thirdparty, "fidelityfx", "bin", "amd_fidelityfx_upscaler_dx12.dll"), targetdir, false)
    end)
rule_end()

-- The one metal-cpp implementation unit: it emits the Objective-C class and selector tables every
-- metal-cpp caller links against, so a binary links exactly one copy. The frameworks are public so a
-- binary linking the backend gets them.
if is_plat("macosx") then
    target("RojoRHIMetalCpp")
        set_kind("static")
        set_default(false)
        release_lto()
        add_files(path.join(component, "Backends/Metal4/MetalCpp/MetalCppImpl.cpp"))
        add_includedirs(path.join(thirdparty, "metal-cpp"))
        add_frameworks("Metal", "MetalFX", "QuartzCore", "Foundation", {public = true})
end

target("RojoRHI")
    set_kind("static")
    warn_unreachable()
    add_options("rojorhi_backend")
    release_lto()
    add_files(path.join(component, "Source/**.cpp"))
    if is_config("rojorhi_backend", "metal4") then
        add_files(path.join(component, "Backends/Metal4/Source/*.cpp"))
        -- The backend's kernels, compiled into a library its sources embed; only this backend needs the Metal toolchain.
        add_rules("rojorhi.metal4_kernels")
        add_files(path.join(component, "Backends/Metal4/Kernels/*.metal"))
        add_includedirs(path.join(thirdparty, "metal-cpp"))
        if is_plat("macosx") then
            add_deps("RojoRHIMetalCpp")
            add_frameworks("CoreGraphics", {public = true})
        end
    elseif is_config("rojorhi_backend", "d3d12") then
        add_files(path.join(component, "Backends/D3D12/Source/*.cpp"))
        -- The backend's kernels, compiled into DXIL its sources embed by the DXC `xmake setup` fetches.
        add_rules("rojorhi.d3d12_kernels")
        add_files(path.join(component, "Backends/D3D12/Kernels/*.hlsl"))
        set_values("rojorhi.dxc", path.join(thirdparty_dir, "dxc/bin/x64/dxc.exe"))
        -- System includes, so the Agility headers shadow the Windows SDK's older d3d12.h and /external:W0 covers them and
        -- the FidelityFX headers the upscaler compiles against; its runtime is loaded beside the executable.
        add_sysincludedirs(path.join(thirdparty, "agility/include"), path.join(thirdparty, "fidelityfx/include"))
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
        d3d12_selection()
        -- user32 for IsWindow, the surface check; shell32 and ole32 for the Program Files folder the PIX capturer is found under.
        add_syslinks("d3d12", "dxgi", "dxguid", "user32", "shell32", "ole32", {public = true})
    -- The Null backend directory holds no source until a part adds one, and xmake warns on an empty glob.
    elseif #os.files(path.join(component_dir, "Backends/Null/Source/*.cpp")) > 0 then
        add_files(path.join(component, "Backends/Null/Source/*.cpp"))
    end
    add_includedirs(path.join(component, "Include"), {public = true})
    add_includedirs(path.join(component, "Source"))
    on_config(function (target)
        local backend = get_config("rojorhi_backend")
        if backend == "metal4" then
            assert(target:is_plat("macosx"), format("rojorhi_backend=metal4 builds on macOS only; this platform is %s, so use null", target:plat()))
        elseif backend == "d3d12" then
            assert(target:is_plat("windows"), format("rojorhi_backend=d3d12 builds on Windows only; this platform is %s, so use null", target:plat()))
        else
            assert(backend == "null", format("rojorhi_backend=%s is not a backend; the values are null, metal4 and d3d12", tostring(backend)))
        end
    end)

target("RojoRHITests")
    set_kind("binary")
    warn_unreachable()
    set_default(false)
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-test")
    add_files(path.join(component, "Tests/*.cpp"))
    local contract_left_out = {}
    local conformance_left_out = {}
    if is_config("rojorhi_backend", "d3d12") then
        -- WARP+GBV: WARP never completes a FidelityFX upscale dispatch under GPU-based validation, while the debug layer alone
        -- runs it, so the two files whose cases submit upscales are left out of that configuration and every other one runs
        -- them; evidence: the floor probe's fsr section, CI run 39463099719.
        if is_config("rojorhi_d3d12_adapter", "warp") and is_config("rojorhi_d3d12_debug", "gbv") then
            table.insert(contract_left_out, "UpscalerTests.cpp")
            table.insert(conformance_left_out, "UpscaleConformanceTests.cpp")
        end
        -- A renamed or deleted file must leave the lists too, so on_config fails on a name that matches no file. The check
        -- runs in script scope, the only scope that can fail a configuration, so the values travel absolute.
        local left_out = {}
        for _, name in ipairs(contract_left_out) do
            table.insert(left_out, path.join(component_dir, "Tests/Contract", name))
        end
        for _, name in ipairs(conformance_left_out) do
            table.insert(left_out, path.join(component_dir, "Tests/Conformance", name))
        end
        set_values("rojorhi.d3d12_left_out", left_out)
        on_config(function (target)
            for _, file in ipairs(table.wrap(target:values("rojorhi.d3d12_left_out"))) do
                assert(os.isfile(file), format("the d3d12 build leaves out %s, which does not exist; remove or rename the entry", file))
            end
        end)
    end
    for _, file in ipairs(os.files(path.join(component_dir, "Tests/Contract/*.cpp"))) do
        if not table.contains(contract_left_out, path.filename(file)) then
            add_files(path.join(component, "Tests/Contract", path.filename(file)))
        end
    end
    add_files(path.join(component, "Tests/Support/*.cpp"))
    -- The conformance directory may hold no source, and a glob that matches nothing makes xmake warn, so each file is added.
    for _, file in ipairs(os.files(path.join(component_dir, "Tests/Conformance/*.cpp"))) do
        if not table.contains(conformance_left_out, path.filename(file)) then
            add_files(path.join(component, "Tests/Conformance", path.filename(file)))
        end
    end
    -- Core and Null cases test the core through the Null backend, which never ships, so they build only with it.
    if is_config("rojorhi_backend", "null") then
        add_files(path.join(component, "Tests/Core/*.cpp"))
        add_files(path.join(component, "Tests/Null/*.cpp"))
        add_files(path.join(component, "Tests/Support/Null/*.cpp"))
    end
    -- Metal 4 cases may reach the backend's private headers and metal-cpp.
    if is_config("rojorhi_backend", "metal4") then
        if #os.files(path.join(component_dir, "Tests/Metal4/*.cpp")) > 0 then
            add_files(path.join(component, "Tests/Metal4/*.cpp"))
        end
        add_files(path.join(component, "Tests/Support/Metal4/*.cpp"))
        add_includedirs(path.join(component, "Backends/Metal4/Source"))
        add_includedirs(path.join(thirdparty, "metal-cpp"))
    end
    -- D3D12 cases may reach the backend's private headers and the Agility headers; the executable loads the runtime
    -- the rule copies beside it.
    if is_config("rojorhi_backend", "d3d12") then
        if #os.files(path.join(component_dir, "Tests/D3D12/*.cpp")) > 0 then
            add_files(path.join(component, "Tests/D3D12/*.cpp"))
        end
        add_files(path.join(component, "Tests/Support/D3D12/*.cpp"))
        add_includedirs(path.join(component, "Backends/D3D12/Source"))
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
        d3d12_selection()
        add_sysincludedirs(path.join(thirdparty, "agility/include"), path.join(thirdparty, "fidelityfx/include"))
        add_rules("rojorhi.d3d12_runtime")
        set_values("rojorhi.thirdparty", thirdparty_dir)
    end
    add_includedirs(path.join(component, "Source"))
    add_includedirs(path.join(component, "Tests"))
    add_deps("RojoRHI")
    add_packages("catch2")
    release_lto()
    -- The tests use the standard tmpfile and getenv, which the MSVC runtime marks as deprecated.
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end
    unreachable_after_stubs()
    if is_config("rojorhi_backend", "metal4") then
        -- Every Metal 4 entry runs under the Metal validation layer, from the test directory.
        local layer = {MTL_DEBUG_LAYER = "1"}
        add_tests("contract", {runargs = {"[contract]~[death],[metal4]~[death]"}, runenvs = layer, realtime_output = true})
        -- Each case spawns this executable again, so keep Catch2's summary in the log and bound a hang.
        add_tests("death", {runargs = {"[death]"}, runenvs = layer, realtime_output = true, run_timeout = 300000})
        add_tests("conformance", {runargs = {"[conformance]"}, runenvs = layer, realtime_output = true})
    elseif is_config("rojorhi_backend", "d3d12") then
        -- contract runs the shared and [d3d12] cases without the death ones, death runs every death case, and
        -- conformance runs the [conformance] cases.
        add_tests("contract", {runargs = {"[contract]~[death],[d3d12]~[death]"}, realtime_output = true})
        -- Each case spawns this executable again, so keep Catch2's summary in the log and bound a hang.
        add_tests("death", {runargs = {"[death]"}, realtime_output = true, run_timeout = 300000})
        add_tests("conformance", {runargs = {"[conformance]"}, realtime_output = true})
    else
        add_tests("contract", {runargs = {"[contract]~[death],[core]~[death],[null]~[death]"}, realtime_output = true})
        -- Each case spawns this executable again, so keep Catch2's summary in the log and bound a hang.
        add_tests("death", {runargs = {"[death]"}, realtime_output = true, run_timeout = 300000})
    end
    -- Conformance shaders compile for every target on any host; the rule needs only the Slang
    -- compiler, which setup fetches. Windows names it slangc.exe.
    add_rules("rojorhi.slang")
    set_values("slang.moduledir", path.join(component_dir, "Shaders/RojoRHI"))
    set_values("slang.slangc", path.join(thirdparty_dir, is_host("windows") and "slang/bin/slangc.exe"
                                                         or "slang/bin/slangc"))
    add_files(path.join(component, "Shaders/Tests/*.slang"))
    -- The probe is among those files, and the layout rule keeps it out of the conformance list: it
    -- compares the shared structs with what each target reflects.
    add_rules("rojorhi.shader_layout")
    set_values("slang.layout_probe", path.join(component_dir, "Shaders/Tests/LayoutProbe.slang"))
    set_values("slang.layout_map", "DrawRoot=rojoRHI::test::DrawRoot")
    set_values("slang.layout_include", "rojoRHI/RHI.h")
    if is_config("rojorhi_backend", "metal4") then
        add_tests("abi", {runargs = {"[abi]"}, runenvs = {MTL_DEBUG_LAYER = "1"}, realtime_output = true})
    else
        add_tests("abi", {runargs = {"[abi]"}, realtime_output = true})
    end

-- A Release probe that Tools/check_lto_inline.py disassembles to show a library entry inlined into
-- the caller. It keeps its symbol table, which Release strips, to name the probe function, and
-- Windows links a program database, the only place dumpbin finds that name.
target("RojoRHILtoProbe")
    set_kind("binary")
    set_default(false)
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-lto-probe")
    add_files(path.join(component, "Tools/LtoProbe/LtoProbe.cpp"))
    add_deps("RojoRHI")
    release_lto()
    set_strip("none")
    if is_plat("windows") then
        set_symbols("debug")
    end
    if is_config("rojorhi_backend", "d3d12") then
        add_rules("rojorhi.d3d12_runtime")
        set_values("rojorhi.thirdparty", thirdparty_dir)
    end
    unreachable_after_stubs()

-- The D3D12 floor probe: two standalone executables from one source that report, as JSON, whether
-- the WARP adapter meets the hardware floor of ADR 0010 and how the D3D12 runtime loaded. The
-- factory variant loads the Agility SDK through a device factory; the exports variant through the
-- D3D12SDKVersion and D3D12SDKPath exports. Each also reports whether the FidelityFX upscaler the
-- runtime rule copies beside it loads and runs on that adapter. They link neither RojoRHI nor Catch2.
if is_plat("windows") then
    for _, variant in ipairs({"Factory", "Exports"}) do
        target("RojoRHID3D12Probe" .. variant)
            set_kind("binary")
            set_default(false)
            set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-d3d12-probe")
            add_files(path.join(component, "Tools/D3D12Probe/D3D12Probe.cpp"))
            if variant == "Exports" then
                add_files(path.join(component, "Tools/D3D12Probe/D3D12ProbeExports.cpp"))
                add_defines("ROJORHI_D3D12_PROBE_EXPORTS")
            end
            -- System includes, so the Agility headers shadow the Windows SDK's older d3d12.h and /external:W0 covers
            -- them and the FidelityFX headers.
            add_sysincludedirs(path.join(thirdparty, "agility/include"), path.join(thirdparty, "fidelityfx/include"))
            add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
            add_syslinks("d3d12", "dxgi", "dxguid", "user32", "version", "advapi32")
            add_rules("rojorhi.d3d12_runtime")
            set_values("rojorhi.thirdparty", thirdparty_dir)
    end
end

-- The benchmark harness under Benchmarks/, one executable so that a process measures RojoRHI and hand-written native
-- code on the same device. It links RojoRHI of the configured backend and runs its own Catch2 session, so it links
-- Catch2 without Catch2Main. Its native arm needs a native API: Metal through metal-cpp on macOS, on every backend
-- there, and D3D12 on a d3d12 build, so it builds on those two only.
if is_plat("macosx") or is_config("rojorhi_backend", "d3d12") then
    target("RojoRHIBench")
        set_kind("binary")
        warn_unreachable()
        set_default(false)
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-bench")
        release_lto()
        add_packages("catch2", {components = "lib"})
        add_includedirs(path.join(component, "Benchmarks"))
        if is_config("rojorhi_backend", "d3d12") then
            -- The Windows host layer and the D3D12 native arm and checks replace Host/*.cpp, Native/ and the two
            -- Metal-only check files; every other source is shared.
            add_deps("RojoRHI")
            add_files(path.join(component, "Benchmarks/Main.cpp"), path.join(component, "Benchmarks/Common/**.cpp"),
                      path.join(component, "Benchmarks/Rojo/**.cpp"),
                      path.join(component, "Benchmarks/Checks/*.cpp|HostChecks.cpp|Metal4Counts.cpp"),
                      path.join(component, "Benchmarks/HostWindows/*.cpp"), path.join(component, "Benchmarks/NativeD3D12/*.cpp"),
                      path.join(component, "Benchmarks/ChecksD3D12/*.cpp"))
            -- An abort or a CRT report ends the process with a message on stderr, never a modal dialog that holds a CI step.
            add_files(path.join(component, "Tests/Support/D3D12/D3D12NoDialog.cpp"))
            add_defines("ROJORHI_BENCH_BACKEND_D3D12", "ROJORHI_BENCH_NATIVE_D3D12", "WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
            d3d12_selection()
            -- System includes, so the Agility headers shadow the Windows SDK's older d3d12.h and /external:W0 covers them.
            add_sysincludedirs(path.join(thirdparty, "agility/include"))
            -- bcrypt for the outputs' SHA-256, version for the runtime DLLs' file versions the environment records.
            add_syslinks("d3d12", "dxgi", "dxguid", "bcrypt", "version")
            add_rules("rojorhi.d3d12_runtime")
            set_values("rojorhi.thirdparty", thirdparty_dir)
            -- The native arm's hand-written kernels, compiled into Shaders/ beside the Slang libraries.
            add_rules("rojorhi.hlsl")
            add_files(path.join(component, "Benchmarks/NativeD3D12/Kernels/*.hlsl"))
            set_values("rojorhi.dxc", path.join(thirdparty_dir, "dxc/bin/x64/dxc.exe"))
        else
            -- The native arm's metal-cpp calls link the one shared metal-cpp unit, the same one a metal4 RojoRHI links.
            add_deps("RojoRHI", "RojoRHIMetalCpp")
            add_sysincludedirs(path.join(thirdparty, "metal-cpp"))
            add_frameworks("Metal", "QuartzCore", "Foundation")
            add_files(path.join(component, "Benchmarks/**.cpp|HostWindows/*.cpp|NativeD3D12/*.cpp|ChecksD3D12/*.cpp"))
            add_defines("ROJORHI_BENCH_NATIVE_METAL")
            if is_config("rojorhi_backend", "metal4") then
                add_includedirs(path.join(component, "Backends/Metal4/Source"))
                add_defines("ROJORHI_BENCH_BACKEND_METAL4")
            else
                add_defines("ROJORHI_BENCH_BACKEND_NULL")
            end
        end
        -- Both shader rules raise on an empty file list, so each is added only when its directory holds a source.
        if #os.files(path.join(component_dir, "Shaders/Bench/*.slang")) > 0 then
            add_rules("rojorhi.slang")
            set_values("slang.moduledir", path.join(component_dir, "Shaders/RojoRHI"))
            set_values("slang.slangc", path.join(thirdparty_dir, is_host("windows") and "slang/bin/slangc.exe"
                                                                 or "slang/bin/slangc"))
            add_files(path.join(component, "Shaders/Bench/*.slang"))
            -- The composed frame's roots and records, checked against Benchmarks/Common/FrameRoots.h and Scene.h.
            add_rules("rojorhi.shader_layout")
            set_values("slang.layout_probe", path.join(component_dir, "Shaders/Bench/BenchFrameProbe.slang"))
            set_values("slang.layout_map", "Transform=rojoRHI::bench::Transform", "MeshVertex=rojoRHI::bench::MeshVertex",
                       "CullRoot=rojoRHI::bench::CullRoot", "GeometryRoot=rojoRHI::bench::GeometryRoot",
                       "ShadowRoot=rojoRHI::bench::ShadowRoot", "AoRoot=rojoRHI::bench::AoRoot",
                       "BlurRoot=rojoRHI::bench::BlurRoot", "LightingRoot=rojoRHI::bench::LightingRoot",
                       "ForwardRoot=rojoRHI::bench::ForwardRoot", "BloomDownRoot=rojoRHI::bench::BloomDownRoot",
                       "BloomUpRoot=rojoRHI::bench::BloomUpRoot", "ToneMapRoot=rojoRHI::bench::ToneMapRoot")
            set_values("slang.layout_include", "rojoRHI/RHI.h", "Common/Scene.h", "Common/FrameRoots.h")
        end
        if is_plat("macosx") and #os.files(path.join(component_dir, "Benchmarks/Native/Kernels/*.metal")) > 0 then
            add_rules("rojorhi.metal")
            add_files(path.join(component, "Benchmarks/Native/Kernels/*.metal"))
        end
        unreachable_after_stubs()
        if is_config("rojorhi_backend", "d3d12") then
            -- The D3D12 debug features are built in, as the build's rojorhi_d3d12_debug selects them; no variable enables them.
            add_tests("host", {runargs = {"check", "[host]"}, realtime_output = true})
            add_tests("native", {runargs = {"check", "[native]"}, realtime_output = true})
        else
            add_tests("null", {runargs = {"check", "[null],[host]"}, realtime_output = true})
            add_tests("native", {runargs = {"check", "[native]"}, runenvs = {MTL_DEBUG_LAYER = "1"}, realtime_output = true})
            if is_config("rojorhi_backend", "metal4") then
                add_tests("metal4", {runargs = {"check", "[metal4]"}, runenvs = {MTL_DEBUG_LAYER = "1"}, realtime_output = true})
            end
        end
end
