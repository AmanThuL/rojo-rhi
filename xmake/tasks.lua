-- Formatting and policy tasks for a standalone configure of this component. A host has tasks of
-- its own and never includes this file, so exactly one `format` and one `policy` exist per root.
-- Paths are relative to this component's root.
task("format")
    set_menu {usage = "xmake format [--check]", description = "clang-format all sources",
              options = {{nil, "check", "k", nil, "check only, do not modify"}}}
    on_run(function ()
        import("core.base.option")
        local args = {"-style=file"}
        table.insert(args, option.get("check") and "--dry-run" or "-i")
        if option.get("check") then table.insert(args, "--Werror") end
        for _, root in ipairs({"Include", "Source", "Backends", "Tests", "Tools"}) do
            for _, f in ipairs(os.files(root .. "/**.h")) do table.insert(args, f) end
            for _, f in ipairs(os.files(root .. "/**.cpp")) do table.insert(args, f) end
        end
        os.execv("clang-format", args)
    end)

task("policy")
    set_menu {usage = "xmake policy", description = "check repository and C++ layout policy"}
    on_run(function ()
        os.execv("python3", {"Tools/check_project_policy.py"})
        os.execv("python3", {"Tools/check_shader_imports.py"})
        os.execv("xmake", {"project", "-k", "compile_commands"})
        os.execv("python3", {"Tools/check_cpp_comments.py", "--public-api-docs", "error"})
        os.execv("python3", {"Tools/check_rhi_headers.py"})
        os.execv("python3", {"Tools/check_cpp_layout.py"})
    end)
