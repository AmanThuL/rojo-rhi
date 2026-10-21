-- Hand-written HLSL -> DXIL for the .hlsl files of a target. A target opts in with add_rules("rojorhi.hlsl") plus its own
-- add_files("<dir>/*.hlsl"), and names the DXC that compiles them with set_values("rojorhi.dxc", <absolute path>), since the
-- project root differs between this component and a host that mounts it.
--
-- Each source compiles as a cs_6_6 kernel with entry `main`, HLSL 2021 and warnings as errors into
-- <targetdir>/Shaders/<name>.dxil, beside the libraries rojorhi.slang writes, so a program loads either kind by name from
-- one directory; DXC signs the container through the dxil.dll beside it. Outputs are flat, so two sources sharing a
-- basename fail the build, and a source must not share a basename with a library rojorhi.slang writes there (a .slang
-- file of one entry writes <name>.dxil), which would overwrite it; nothing checks that.
-- The flags do not depend on the build mode. A missing DXC fails the build, naming
-- `xmake setup`, which fetches the pinned one; a failed compile fails the build with the compiler's own output.
--
-- The work runs in before_build, and each output is skipped while it is newer than its own source, this file and the DXC
-- binary, so a new pin compiles every kernel again. Nothing else is compared, so a file the source includes does not
-- rebuild it; delete the .dxil (or the target directory) after editing one. A missing DXC fails only when an output has
-- work to do.
rule("rojorhi.hlsl")
    set_extensions(".hlsl")

    before_build(function (target)
        local batch = target:sourcebatches()["rojorhi.hlsl"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)
        if #sources == 0 then
            os.raise("rojorhi.hlsl: target '%s' has no .hlsl files", target:name())
        end
        local dxc = target:values("rojorhi.dxc")
        if not dxc then
            os.raise("rojorhi.hlsl: target '%s' sets no rojorhi.dxc, the path of the DXC that compiles its kernels", target:name())
        end

        -- Windows and the default macOS file system fold case, so the basename comparison does too.
        local owners = {}
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local key = name:lower()
            if owners[key] then
                os.raise("rojorhi.hlsl: '%s' and '%s' share output basename '%s'", owners[key], source, name)
            end
            owners[key] = source
        end

        local rulefile = path.join(os.scriptdir(), "hlsl.lua")
        local outdir = path.join(target:targetdir(), "Shaders")
        os.mkdir(outdir)
        for _, source in ipairs(sources) do
            local dxil = path.join(outdir, path.basename(source) .. ".dxil")
            local mtime = os.isfile(dxil) and os.mtime(dxil) or nil
            local stale = not mtime or mtime < os.mtime(source) or mtime < os.mtime(rulefile)
                          or (os.isfile(dxc) and mtime < os.mtime(dxc))
            if stale then
                if not os.isfile(dxc) then
                    os.raise("rojorhi.hlsl: %s is missing, which compiles the target's HLSL kernels; run `xmake setup`", dxc)
                end
                cprint("${color.build.object}dxc %s", source)
                os.vrunv(dxc, {"-T", "cs_6_6", "-E", "main", "-HV", "2021", "-WX", "-Fo", dxil, source})
            end
        end
    end)
