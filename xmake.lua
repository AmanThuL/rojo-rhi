-- Project root for a standalone configure of the RHI component. It carries root settings only:
-- every target lives in xmake/targets.lua, which a host project includes instead of this file.
-- xmake resolves a project root to the outermost ancestor holding an xmake.lua, so a configure
-- from inside a host checkout names this directory explicitly: `xmake f -P RHI`.
set_project("RHI")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_defaultmode("debug")
set_warnings("allextra")
set_policy("build.warning", true)

add_requires("catch2 3.x", "glm")

includes("xmake/setup.lua", "xmake/targets.lua")
