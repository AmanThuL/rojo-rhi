-- Slang -> readable MSL -> .metallib for this component's own smoke shaders. Emits both artifacts
-- into <targetdir>/Shaders/; a target opts in with add_rules("rojorhi_slang2metallib") plus its own
-- add_files("<dir>/**.slang"). The rule reads nothing outside the target: the Slang compiler and
-- the module directory arrive as target values, because the project root differs between this
-- component's own root and a host that mounts it.
--
-- A host may carry a rule of its own over a different shader tree. The names differ, so including
-- both is not a redefinition, and the target-directory guard below looks for either.
--
-- Every check is scoped to the target's own Slang sources, so a target compiling a different
-- shader tree is neither constrained by nor rebuilt for a tree it never reads.
rule("rojorhi_slang2metallib")
    set_extensions(".slang")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        -- Two targets emitting the same shader paths race under a parallel build. The rule
        -- rejects a shared target directory before either compiler can write partial output.
        import("core.project.project")
        for _, other in pairs(project.targets()) do
            if other:name() ~= target:name()
               and (other:rule("rojorhi_slang2metallib") or other:rule("slang2metallib"))
               and path.absolute(other:targetdir()) == path.absolute(target:targetdir()) then
                os.raise("rojorhi_slang2metallib: targets '%s' and '%s' share targetdir '%s'; give one "
                         .. "its own set_targetdir", target:name(), other:name(), target:targetdir())
            end
        end

        -- This target's own .slang sources, keyed by the rule name that batched them. Both the
        -- basename guard and the dependency list below work from this one list, because the
        -- shared output directory and the stale-artifact risk are per target, not repository-wide.
        local batch = target:sourcebatches()["rojorhi_slang2metallib"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)

        -- Runtime paths are flat even though sources are nested. Also reject case aliases on
        -- the default macOS filesystem before any command can overwrite another artifact.
        local names = {}
        for _, shader in ipairs(sources) do
            local basename = path.basename(shader):lower()
            if names[basename] then
                os.raise("rojorhi_slang2metallib: '%s' and '%s' share output basename '%s'",
                         names[basename], shader, basename)
            end
            names[basename] = shader
        end

        -- Both values are absolute and supplied by the target, so neither depends on which
        -- directory is the project root.
        local moduledir = assert(target:values("slang.moduledir"),
                                 "rojorhi_slang2metallib: set_values(\"slang.moduledir\", <dir>)")
        local slangc = assert(target:values("slang.slangc"),
                              "rojorhi_slang2metallib: set_values(\"slang.slangc\", <slangc>)")
        local outdir = path.join(target:targetdir(), "Shaders")
        local name = path.basename(sourcefile)
        local msl = path.join(outdir, name .. ".metal")
        batchcmds:mkdir(outdir)
        batchcmds:show_progress(opt.progress, "${color.build.object}slang %s", sourcefile)
        batchcmds:vrunv(slangc, {sourcefile, "-I", moduledir, "-target", "metal", "-o", msl})
        -- Offline metallib precompile is optional because Command Line Tools installations may
        -- not include the Metal toolchain; the runtime can compile the emitted MSL instead.
        local has_metal = try {function ()
            os.runv("xcrun", {"-sdk", "macosx", "metal", "--version"}); return true
        end}
        if has_metal then
            local lib = path.join(outdir, name .. ".metallib")
            batchcmds:vrunv("xcrun", {"-sdk", "macosx", "metal", "-std=metal4.0",
                                      "-frecord-sources", "-gline-tables-only", "-o", lib, msl})
        end
        -- Every .slang source of this target, plus every module under its include directory, and
        -- deliberately so: `import Shadow;` makes ShadowSmoke.slang depend on Shadow.slang, and
        -- nothing here can see that edge -- slangc's CLI has no depfile mode wired up, and parsing
        -- `import` lines out of the source would be a second, silently-drifting implementation of
        -- Slang's module resolution. Without this, editing a module leaves every importer's
        -- .metal/.metallib stale. The module directory is listed separately because a module is an
        -- input the compiler resolves through -I, not a source the target compiles: a target whose
        -- pattern does not happen to sweep its own modules in would otherwise miss them entirely.
        -- The conservative list rebuilds all of the target's shaders whenever any entry or module
        -- it can reach changes, without duplicating the compiler's dependency resolver.
        local depfiles = {}
        for _, shader in ipairs(sources) do
            depfiles[path.absolute(shader, os.projectdir())] = true
        end
        for _, module in ipairs(os.files(path.join(moduledir, "**.slang"))) do
            depfiles[path.absolute(module)] = true
        end
        local dependencies = table.orderkeys(depfiles)
        batchcmds:add_depfiles(dependencies)
        batchcmds:set_depmtime(os.mtime(msl))
        batchcmds:set_depcache(target:dependfile(msl))
    end)
