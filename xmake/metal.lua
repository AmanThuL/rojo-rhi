-- Hand-written MSL -> .metallib for the .metal files of a target. A target opts in with
-- add_rules("rojorhi.metal") plus its own add_files("<dir>/*.metal"); the rule needs no other value.
--
-- Each source compiles with `xcrun -sdk macosx metal -std=metal4.0` into <targetdir>/Shaders/<name>.metallib,
-- beside the libraries rojorhi.slang writes, so a program loads either kind by name from one directory. Outputs
-- are flat, so two sources sharing a basename fail the build. Without a Metal toolchain (`xcrun -f metal`
-- fails) the rule prints a notice and compiles nothing, and the build still succeeds; a failed compile fails
-- the build with the compiler's own output.
--
-- The work runs in before_build, and each output is skipped while it is newer than its own source. Nothing else
-- is compared, so a header the source includes, or a new compiler, does not rebuild it; delete the .metallib
-- (or the target directory) after editing one.
rule("rojorhi.metal")
    set_extensions(".metal")

    before_build(function (target)
        local batch = target:sourcebatches()["rojorhi.metal"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)
        if #sources == 0 then
            os.raise("rojorhi.metal: target '%s' has no .metal files", target:name())
        end

        -- The default macOS file system folds case, so the basename comparison does too.
        local owners = {}
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local key = name:lower()
            if owners[key] then
                os.raise("rojorhi.metal: '%s' and '%s' share output basename '%s'", owners[key], source, name)
            end
            owners[key] = source
        end

        local has_metal = is_host("macosx") and try {function ()
            os.runv("xcrun", {"-f", "metal"}); return true
        end}
        if not has_metal then
            cprint("${yellow}metal: metallib skipped, xcrun -f metal found no Metal toolchain")
            return
        end

        local outdir = path.join(target:targetdir(), "Shaders")
        os.mkdir(outdir)
        for _, source in ipairs(sources) do
            local lib = path.join(outdir, path.basename(source) .. ".metallib")
            if not os.isfile(lib) or os.mtime(lib) < os.mtime(source) then
                cprint("${color.build.object}metal %s", source)
                os.runv("xcrun", {"-sdk", "macosx", "metal", "-std=metal4.0", "-o", lib, source})
            end
        end
    end)
