-- Test-only Dear ImGui target for the standalone root, which sets `rojorhi_imgui_target` to it. A host
-- project supplies its own ImGui target through that variable and never includes this file.
-- The sources come from ThirdParty/imgui, which `xmake setup` fetches at its pin.
local component_dir = path.directory(os.scriptdir())
local thirdparty_dir = rojorhi_thirdparty and path.absolute(rojorhi_thirdparty)
                       or path.join(component_dir, "ThirdParty")
local imgui_dir = path.relative(path.join(thirdparty_dir, "imgui"), os.scriptdir())

target("RojoRHIDearImGui")
    set_kind("static")
    set_default(false)
    set_warnings("none")
    for _, file in ipairs({"imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp"}) do
        add_files(path.join(imgui_dir, file))
    end
    -- A system include directory, so imgui.h raises no warning under MSVC /WX in dependants.
    add_sysincludedirs(imgui_dir, {public = true})
