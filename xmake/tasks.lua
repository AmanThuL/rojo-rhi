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
        -- Every top-level directory of C++ sources but the fetched, built and example trees.
        local skipped = {ThirdParty = true, build = true, Examples = true}
        for _, root in ipairs(os.dirs("*")) do
            if not skipped[path.filename(root)] then
                for _, f in ipairs(os.files(root .. "/**.h")) do table.insert(args, f) end
                for _, f in ipairs(os.files(root .. "/**.cpp")) do table.insert(args, f) end
            end
        end
        os.execv("clang-format", args)
    end)

-- The policy task runs the repository's internal checks, which a public export omits with dev_tasks.lua.
if os.isfile(path.join(os.scriptdir(), "dev_tasks.lua")) then
    includes("dev_tasks.lua")
end
