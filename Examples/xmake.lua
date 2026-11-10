-- The examples: one executable per directory, each with its own main.cpp and Slang shaders, sharing
-- only the window in Common/. They build on the metal4 and d3d12 backends, with
-- `xmake f --rojorhi_examples=y`, and need GLFW, which xmake fetches.
option("rojorhi_examples")
    set_default(false)
    set_showmenu(true)
    set_description("Build the examples under Examples/ (metal4 or d3d12; fetches GLFW)")
option_end()

if not has_config("rojorhi_examples") or is_config("rojorhi_backend", "null") then
    return
end

add_requires("glfw 3.4")

local examples_dir = os.scriptdir()
local component_dir = path.directory(examples_dir)
local thirdparty_dir = path.join(component_dir, "ThirdParty")

target("RojoRHIExampleCommon")
    set_kind("static")
    set_default(false)
    add_files("Common/*.cpp")
    add_includedirs(".", {public = true})
    add_deps("RojoRHI")
    add_packages("glfw", {public = true})
    if is_plat("macosx") then
        add_frameworks("Cocoa", "QuartzCore", {public = true})
    end

for _, dir in ipairs(os.dirs(path.join(examples_dir, "*_*"))) do
    local name = path.filename(dir)
    target(name)
        set_kind("binary")
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/examples/" .. name)
        add_files(path.join(name, "*.cpp"))
        add_deps("RojoRHIExampleCommon")
        add_rules("rojorhi.slang")
        add_files(path.join(name, "*.slang"))
        set_values("slang.moduledir", path.join(component_dir, "Shaders/RojoRHI"))
        set_values("slang.slangc", path.join(thirdparty_dir, is_host("windows") and "slang/bin/slangc.exe" or "slang/bin/slangc"))
        set_values("slang.embed", "Shaders.h")
        if is_mode("release") then
            set_policy("build.optimization.lto", true)
        end
        if is_config("rojorhi_backend", "d3d12") then
            add_rules("rojorhi.d3d12_runtime")
            set_values("rojorhi.thirdparty", thirdparty_dir)
            add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
            if is_mode("release") then
                add_cxflags("/wd4702", "/wd4722")
            end
        end
    target_end()
end
