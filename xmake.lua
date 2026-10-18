-- Project root for a standalone configure of the RHI component. It carries root settings only:
-- every target lives in xmake/targets.lua, which a host project includes instead of this file.
-- xmake resolves a project root to the outermost ancestor holding an xmake.lua, so a configure
-- from inside a host checkout names this directory explicitly: `xmake f -P RojoRHI`.
set_project("RojoRHI")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_defaultmode("debug")
set_warnings("allextra")
set_policy("build.warning", true)
-- MSVC warnings fail the build; hosted Windows CI is the only MSVC build there is.
if is_plat("windows") then
    add_cxflags("/WX")
end

add_requires("catch2 3.x", "glm")

includes("xmake/setup.lua", "xmake/tasks.lua", "xmake/targets.lua")
