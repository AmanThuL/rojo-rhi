-- Slang -> MSL, SPIR-V, HLSL (and, where the host can, .metallib and DXIL) for the shader entry files
-- of a target in the new interface tree. A target opts in with add_rules("rojorhi.slang") plus its own
-- add_files("<dir>/*.slang"), and supplies two absolute values, because the project root differs
-- between this component's own root and a host that mounts it:
--
--   slang.slangc      the Slang compiler
--   slang.moduledir   the directory `import` resolves through (-I)
--
-- Outputs land in <targetdir>/Shaders/<name>.{metal,spv,hlsl} plus .metallib and .dxil when the host
-- has the toolchain, and the basenames are written to $(builddir)/rojorhi/ShaderList.h as
-- `kConformanceShaders` for a test to enumerate. Any failed compile fails the build with the
-- compiler's own output. This rule is separate from the v1 rule of shaders.lua, which Legacy/ keeps.
--
-- The work runs in before_build so the generated header exists before the target's C++ compiles. Each
-- output is skipped while it is newer than every entry, every module and the compiler.

-- Script scope does not see this file's locals, so the helpers live inside the callback.
rule("rojorhi.slang")
    set_extensions(".slang")

    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi"))
    end)

    before_build(function (target)
        import("core.project.config")
        -- Entry points of a source file, read from its [shader("stage")] attributes with comments removed.
        -- Each result is {name = <function>, stage = <stage>}. A file with several attributes compiles every
        -- entry; with none, the compiler decides what to do and the list is empty.
        local function entry_points(source)
            local text = io.readfile(source):gsub("/%*.-%*/", ""):gsub("//[^\n]*", "")
            local entries = {}
            local init = 1
            while true do
                local first, last, stage = text:find("%[%s*shader%s*%(%s*\"(%w+)\"%s*%)%s*%]", init)
                if not first then break end
                -- After the attribute come further bracketed attributes, then the return type and
                -- the function name.
                local tail = text:sub(last + 1)
                local stripped = tail:gsub("^%s*%b[]", "")
                while stripped ~= tail do
                    tail = stripped
                    stripped = tail:gsub("^%s*%b[]", "")
                end
                local name = tail:match("^%s*[%w_<>:,]+%s+([%w_]+)%s*%(")
                assert(name, format("rojorhi.slang: no function follows the [shader(\"%s\")] attribute in %s",
                                    stage, source))
                table.insert(entries, {name = name, stage = stage})
                init = last + 1
            end
            return entries
        end

        -- Runs slangc and raises with its own output when it fails. `stdout_file` captures standard output
        -- into a file, which is how the HLSL target delivers every entry point of a file in one text.
        -- Returns whether it succeeded plus the error text, when `soft` asks to handle the failure.
        local function run_slangc(slangc, args, stdout_file, soft)
            local errfile = os.tmpfile()
            local ok = try {function ()
                os.execv(slangc, args, {stdout = stdout_file, stderr = errfile})
                return true
            end}
            local err = os.isfile(errfile) and io.readfile(errfile) or ""
            os.tryrm(errfile)
            if not ok and not soft then
                if stdout_file then os.tryrm(stdout_file) end
                os.raise("rojorhi.slang: slangc failed for %s\n%s", table.concat(args, " "), err)
            end
            return ok and true or false, err
        end

        local moduledir = assert(target:values("slang.moduledir"),
                                 "rojorhi.slang: set_values(\"slang.moduledir\", <dir>)")
        local slangc = assert(target:values("slang.slangc"),
                              "rojorhi.slang: set_values(\"slang.slangc\", <slangc>)")
        assert(os.isfile(slangc), format("rojorhi.slang: %s is missing; run `xmake setup`", slangc))

        local batch = target:sourcebatches()["rojorhi.slang"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)

        if #sources == 0 then
            os.raise("rojorhi.slang: target '%s' has no .slang entry files", target:name())
        end

        -- Outputs are flat while sources may be nested, and the default macOS file system folds
        -- case, so two entries sharing a basename would overwrite each other's artifacts.
        local names, owners = {}, {}
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local key = name:lower()
            if owners[key] then
                os.raise("rojorhi.slang: '%s' and '%s' share output basename '%s'",
                         owners[key], source, name)
            end
            owners[key] = source
            table.insert(names, name)
        end

        -- The header lists the entry files; rewritten only when it changes, so a no-op build does
        -- not recompile the test.
        local lines = {"// Generated by rojorhi.slang; do not edit.", "#pragma once", "",
                       "#include <string_view>", "",
                       "inline constexpr std::string_view kConformanceShaders[] = {"}
        for _, name in ipairs(names) do
            table.insert(lines, format("    \"%s\",", name))
        end
        table.insert(lines, "};")
        local header = path.join(config.builddir(), "rojorhi", "ShaderList.h")
        local content = table.concat(lines, "\n") .. "\n"
        if not os.isfile(header) or io.readfile(header) ~= content then
            io.writefile(header, content)
        end

        -- Anything a compile reads: the entries, every module under -I, the compiler.
        local newest = os.mtime(slangc)
        for _, file in ipairs(table.join(sources, os.files(path.join(moduledir, "**.slang")))) do
            newest = math.max(newest, os.mtime(file))
        end
        local function stale(output)
            return not os.isfile(output) or os.mtime(output) < newest
        end

        local outdir = path.join(target:targetdir(), "Shaders")
        os.mkdir(outdir)
        local has_metal = is_host("macosx") and try {function ()
            os.runv("xcrun", {"-f", "metal"}); return true
        end}
        -- DXIL needs a DXC the compiler can load, so one known-good compile decides, once per build,
        -- whether DXIL is attempted at all. Without a DXC, slangc (v2026.14.1, macOS) exits non-zero
        -- with, among other lines: "error[E00100]: failed to load downstream compiler 'dxc'". Only
        -- this probe may turn a failure into a skip: when it succeeds, every DXIL failure on a real
        -- shader fails the build.
        local probe = path.join(outdir, "DxilProbe.slang")
        io.writefile(probe, "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid probeMain() {}\n")
        local dxil_available, probe_err = run_slangc(slangc, {probe, "-target", "dxil", "-entry",
            "probeMain", "-stage", "compute", "-o", path.join(outdir, "DxilProbe.dxil")}, nil, true)
        os.tryrm(probe)
        os.tryrm(path.join(outdir, "DxilProbe.dxil"))
        if not dxil_available then
            local reason = probe_err:match("[^\n]*failed to load[^\n]*") or probe_err:match("[^\n]+") or "unknown"
            cprint("${yellow}slang: DXIL skipped, the probe compile failed (%s)", reason)
        end
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local entries = entry_points(source)
            local base = {source, "-I", moduledir}
            cprint("${color.build.object}slang %s", source)

            local msl = path.join(outdir, name .. ".metal")
            if stale(msl) then
                run_slangc(slangc, table.join(base, {"-target", "metal", "-o", msl}))
            end
            local spv = path.join(outdir, name .. ".spv")
            if stale(spv) then
                run_slangc(slangc, table.join(base, {"-target", "spirv", "-o", spv}))
            end
            -- HLSL has no single-output form for several entries, so its text is the compiler's
            -- standard output, which holds every entry of the file.
            local hlsl = path.join(outdir, name .. ".hlsl")
            if stale(hlsl) then
                run_slangc(slangc, table.join(base, {"-target", "hlsl"}), hlsl)
            end

            if has_metal then
                local lib = path.join(outdir, name .. ".metallib")
                if stale(lib) then
                    os.runv("xcrun", {"-sdk", "macosx", "metal", "-std=metal4.0", "-o", lib, msl})
                end
            end

            -- DXIL needs an entry and stage per output; a failure here is a real error.
            for _, entry in ipairs(entries) do
                local dxil = path.join(outdir, #entries == 1 and (name .. ".dxil")
                                       or format("%s.%s.dxil", name, entry.name))
                if dxil_available and stale(dxil) then
                    run_slangc(slangc, table.join(base, {"-target", "dxil", "-entry",
                        entry.name, "-stage", entry.stage, "-o", dxil}))
                end
            end
        end
    end)
