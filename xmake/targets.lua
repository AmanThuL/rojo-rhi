-- Every target of the RHI component, includable from this component's own root and from a host
-- project that mounts the component. The file is the component's build interface: a host includes
-- it and nothing else, so project settings, language level and package requires stay declared once
-- per root rather than once per include.
--
-- The shader rules the test targets opt into, and the rule that compiles the Metal 4 backend's own
-- kernels, are part of that interface, so this file includes them rather than leaving a host to
-- remember more includes.
includes("shaders.lua", "slang.lua", "metal.lua", "kernels.lua")

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
option("rojorhi_backend")
    set_default("null")
    set_values("null", "metal4")
    set_showmenu(true)
    set_description("Backend for RojoRHI: null, the GPU-free test backend, or metal4, which builds on macOS only")
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
        end
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
        else
            assert(backend == "null", format("rojorhi_backend=%s is not a backend; the values are null and metal4", tostring(backend)))
        end
    end)

target("RojoRHITests")
    set_kind("binary")
    set_default(false)
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-test")
    add_files(path.join(component, "Tests/*.cpp"))
    if is_config("rojorhi_backend", "metal4") then
        -- The contract files whose chapters the Metal 4 backend does not implement yet. A file leaves the list when it
        -- passes whole, and the list ends empty.
        local metal4_pending = {
            "AliasingDeathTests.cpp", "AliasingTests.cpp", "BarrierDeathTests.cpp", "BarrierTests.cpp",
            "CopyDeathTests.cpp", "CopyTests.cpp", "CounterTests.cpp", "DiagnosticsDeathTests.cpp",
            "DiagnosticsTests.cpp", "DrawDeathTests.cpp", "DrawTests.cpp", "HandoffDeathTests.cpp",
            "HandoffTests.cpp", "HazardDeathTests.cpp", "HazardTests.cpp", "IndirectHazardDeathTests.cpp",
            "IndirectHazardTests.cpp", "LifetimeDeathTests.cpp", "LifetimeTests.cpp", "PipelineDeathTests.cpp",
            "PipelineTests.cpp", "RenderPassDeathTests.cpp", "RenderPassTests.cpp",
            "SparseDeathTests.cpp", "SparseOrderDeathTests.cpp", "SparseOrderTests.cpp", "SparseTests.cpp",
            "SwapchainDeathTests.cpp", "SwapchainTests.cpp", "TrackerDeathTests.cpp", "TrackerTests.cpp",
            "UpscaleDeathTests.cpp", "UpscalerTests.cpp",
        }
        local pending = {}
        for _, name in ipairs(metal4_pending) do
            pending[name] = true
        end
        -- A renamed or deleted contract file must leave the list too, so on_config fails on a name that matches no file.
        -- The check runs in script scope, the only scope that can fail a configuration, so the values travel absolute.
        set_values("rojorhi.metal4_pending", metal4_pending)
        set_values("rojorhi.contract_dir", path.join(component_dir, "Tests/Contract"))
        on_config(function (target)
            local contract_dir = target:values("rojorhi.contract_dir")
            for _, name in ipairs(table.wrap(target:values("rojorhi.metal4_pending"))) do
                assert(os.isfile(path.join(contract_dir, name)),
                       format("metal4_pending names Tests/Contract/%s, which does not exist; remove or rename the entry", name))
            end
        end)
        for _, file in ipairs(os.files(path.join(component_dir, "Tests/Contract/*.cpp"))) do
            if not pending[path.filename(file)] then
                add_files(path.join(component, "Tests/Contract", path.filename(file)))
            end
        end
    else
        add_files(path.join(component, "Tests/Contract/*.cpp"))
    end
    add_files(path.join(component, "Tests/Support/*.cpp"))
    -- The conformance directory holds no source until a part adds one, and xmake warns on an empty glob.
    if #os.files(path.join(component_dir, "Tests/Conformance/*.cpp")) > 0 then
        add_files(path.join(component, "Tests/Conformance/*.cpp"))
    end
    -- Core and Null cases test the core through the Null backend, which never ships, so they build only with it.
    if not is_config("rojorhi_backend", "metal4") then
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
    add_includedirs(path.join(component, "Source"))
    add_includedirs(path.join(component, "Tests"))
    add_deps("RojoRHI")
    add_packages("catch2")
    release_lto()
    -- The tests use the standard tmpfile and getenv, which the MSVC runtime marks as deprecated.
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_WARNINGS")
    end
    if is_config("rojorhi_backend", "metal4") then
        -- Every Metal 4 entry runs under the Metal validation layer, from the test directory.
        local layer = {MTL_DEBUG_LAYER = "1"}
        add_tests("contract", {runargs = {"[contract]~[death],[metal4]~[death]"}, runenvs = layer})
        -- Each case spawns this executable again, so keep Catch2's summary in the log and bound a hang.
        add_tests("death", {runargs = {"[death]"}, runenvs = layer, realtime_output = true, run_timeout = 300000})
        add_tests("conformance", {runargs = {"[conformance]"}, runenvs = layer})
    else
        add_tests("contract", {runargs = {"[contract]~[death],[core]~[death],[null]~[death]"}})
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
        add_tests("abi", {runargs = {"[abi]"}, runenvs = {MTL_DEBUG_LAYER = "1"}})
    else
        add_tests("abi", {runargs = {"[abi]"}})
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

-- The benchmark harness under Benchmarks/, one executable so that a process measures RojoRHI and hand-written
-- metal-cpp code on the same device. It links RojoRHI of the configured backend and runs its own Catch2 session, so
-- it links Catch2 without Catch2Main. Its native code needs Metal, so it builds on macOS only.
if is_plat("macosx") then
    target("RojoRHIBench")
        set_kind("binary")
        set_default(false)
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/rojorhi-bench")
        release_lto()
        -- The native arm's metal-cpp calls link the one shared metal-cpp unit, the same one a metal4 RojoRHI links.
        add_deps("RojoRHI", "RojoRHIMetalCpp")
        add_packages("catch2", {components = "lib"})
        add_includedirs(path.join(component, "Benchmarks"))
        add_sysincludedirs(path.join(thirdparty, "metal-cpp"))
        add_frameworks("Metal", "QuartzCore", "Foundation")
        add_files(path.join(component, "Benchmarks/**.cpp"))
        if is_config("rojorhi_backend", "metal4") then
            add_includedirs(path.join(component, "Backends/Metal4/Source"))
            add_defines("ROJORHI_BENCH_BACKEND_METAL4")
        else
            add_defines("ROJORHI_BENCH_BACKEND_NULL")
        end
        -- Both shader rules raise on an empty file list, so each is added only when its directory holds a source.
        if #os.files(path.join(component_dir, "Shaders/Bench/*.slang")) > 0 then
            add_rules("rojorhi.slang")
            set_values("slang.moduledir", path.join(component_dir, "Shaders/RojoRHI"))
            set_values("slang.slangc", path.join(thirdparty_dir, "slang/bin/slangc"))
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
        if #os.files(path.join(component_dir, "Benchmarks/Native/Kernels/*.metal")) > 0 then
            add_rules("rojorhi.metal")
            add_files(path.join(component, "Benchmarks/Native/Kernels/*.metal"))
        end
        add_tests("null", {runargs = {"check", "[null],[host]"}, realtime_output = true})
        add_tests("native", {runargs = {"check", "[native]"}, runenvs = {MTL_DEBUG_LAYER = "1"}, realtime_output = true})
        if is_config("rojorhi_backend", "metal4") then
            add_tests("metal4", {runargs = {"check", "[metal4]"}, runenvs = {MTL_DEBUG_LAYER = "1"}, realtime_output = true})
        end
end
