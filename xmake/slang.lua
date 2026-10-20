-- Slang -> MSL, SPIR-V, HLSL (and, where the host can, .metallib and DXIL) for the shader entry files
-- of a target in the new interface tree. A target opts in with add_rules("rojorhi.slang") plus its own
-- add_files("<dir>/*.slang"), and supplies two absolute values, because the project root differs
-- between this component's own root and a host that mounts it:
--
--   slang.slangc      the Slang compiler
--   slang.moduledir   the directory `import` resolves through (-I)
--
-- Outputs land in <targetdir>/Shaders/<name>.{metal,spv,hlsl} plus <name>.metal.json, the reflection of the
-- MSL compile, and .metallib and .dxil when the host has the toolchain; a d3d12 configure requires DXIL and
-- fails without a DXC beside slangc. Beside each .metallib the rule writes <name>.metal4lib, a RojoRHI
-- shader library: the metallib behind a text header with the
-- ROJORHI_SHADER_ABI_VERSION stamp and each entry's stage and threadgroup size, which
-- Tools/shader_library.py packages from the reflection and the SPIR-V module. A host without the Metal
-- toolchain builds no .metal4lib. Where DXIL is built, the rule also writes <name>.d3d12lib, the same kind of library
-- around each entry's signed DXIL (shader_library.py --format d3d12). Metallibs target the macOS 26
-- deployment floor. The basenames are written to $(builddir)/rojorhi/<target>/ShaderList.h as
-- `kConformanceShaders` for a test to enumerate, with `kMetalLibrariesBuilt` and `kD3D12LibrariesBuilt`
-- saying whether the .metal4lib and .d3d12lib files exist. The directory is per target, so two targets
-- using the rule each include their own list. Any failed compile fails the build with
-- the compiler's own output, and a successful compile's warnings, `metal`'s included, are printed. An entry
-- named `main` fails the MSL compile, which would rename it. This rule is separate from the v1 rule of
-- shaders.lua, which Legacy/ keeps.
--
-- Rule "rojorhi.shader_layout", declared below, checks C++ struct layouts against the reflection of a probe.
--
-- The work runs in before_build so the generated header exists before the target's C++ compiles. Each
-- output is skipped while it is newer than every entry, every module, the compiler and this rule. A shader
-- library is also rebuilt when its metallib or DXIL, reflection, SPIR-V or the packaging tool is newer.

-- Script scope does not see this file's locals, so the helpers live inside the callback.
rule("rojorhi.slang")
    set_extensions(".slang")

    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi", target:name()))
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

        -- Runs `program` (slangc, xcrun or python3) and raises with its own output, under `label`, when it
        -- fails. `stdout_file` captures standard output into a file, which is how the HLSL target delivers
        -- every entry point of a file in one text. Returns whether it succeeded plus the error text, when
        -- `soft` asks to handle the failure. A successful run's own warnings are printed, not hidden.
        local function run_tool(program, label, args, stdout_file, soft)
            local errfile = os.tmpfile()
            local ok = try {function ()
                os.execv(program, args, {stdout = stdout_file, stderr = errfile})
                return true
            end}
            local err = os.isfile(errfile) and io.readfile(errfile) or ""
            os.tryrm(errfile)
            if not ok and not soft then
                if stdout_file then os.tryrm(stdout_file) end
                os.raise("rojorhi.slang: %s failed for %s\n%s", label, table.concat(args, " "), err)
            end
            if ok and not soft and err:find("%S") then
                cprint("${yellow}%s", err)
            end
            return ok and true or false, err
        end

        local moduledir = assert(target:values("slang.moduledir"),
                                 "rojorhi.slang: set_values(\"slang.moduledir\", <dir>)")
        local slangc = assert(target:values("slang.slangc"),
                              "rojorhi.slang: set_values(\"slang.slangc\", <slangc>)")
        assert(os.isfile(slangc), format("rojorhi.slang: %s is missing; run `xmake setup`", slangc))

        -- Layout probes (see rojorhi.shader_layout) have no entry point of their own to ship, so they are not
        -- conformance shaders.
        local probes = {}
        for _, probe in ipairs(table.wrap(target:values("slang.layout_probe"))) do
            probes[path.absolute(probe)] = true
        end
        local batch = target:sourcebatches()["rojorhi.slang"]
        local sources = {}
        for _, source in ipairs(table.unique(table.wrap(batch and batch.sourcefiles))) do
            if not probes[path.absolute(source)] then
                table.insert(sources, source)
            end
        end
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

        -- Metallibs and the shader libraries around them need the Metal toolchain, which only macOS has.
        local has_metal = is_host("macosx") and try {function ()
            os.runv("xcrun", {"-f", "metal"}); return true
        end}
        if has_metal then
            cprint("${green}slang: metallib enabled (xcrun metal)")
        elseif is_host("macosx") then
            cprint("${yellow}slang: metallib skipped, `xcrun -f metal` found no Metal toolchain")
        else
            cprint("${yellow}slang: metallib skipped, the host is not macOS")
        end

        -- Anything a compile reads: the entries, every module under -I, the compiler, and this rule for its
        -- flags. The rule sits in xmake/ beside Shaders/, as Tools/ does.
        local root = path.directory(path.directory(moduledir))
        local rule_file = path.join(root, "xmake", "slang.lua")
        assert(os.isfile(rule_file), format("rojorhi.slang: %s is missing", rule_file))
        local newest = math.max(os.mtime(slangc), os.mtime(rule_file))
        for _, file in ipairs(table.join(sources, os.files(path.join(moduledir, "**.slang")))) do
            newest = math.max(newest, os.mtime(file))
        end
        local function stale(output)
            return not os.isfile(output) or os.mtime(output) < newest
        end
        -- slangc (v2026.14.1) leaves an -o file untouched when its new text is identical, which would keep
        -- it stale on every later build, so a compile first removes the outputs it rewrites.
        local function clear(...)
            for _, output in ipairs({...}) do
                os.tryrm(output)
            end
        end

        local outdir = path.join(target:targetdir(), "Shaders")
        os.mkdir(outdir)
        -- DXIL needs a DXC the compiler can load, so one known-good compile decides, once per build,
        -- whether DXIL is attempted at all. Without a DXC, slangc (v2026.14.1, macOS) exits non-zero
        -- with, among other lines: "error[E00100]: failed to load downstream compiler 'dxc'". Only
        -- this probe may turn a failure into a skip: when it succeeds, every DXIL failure on a real
        -- shader fails the build. A d3d12 configure has no skip: the probe failing fails the build.
        local probe = path.join(outdir, "DxilProbe.slang")
        io.writefile(probe, "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid probeMain() {}\n")
        local dxil_available, probe_err = run_tool(slangc, "slangc", {probe, "-D", "ROJORHI_TARGET_HLSL",
            "-target", "dxil", "-entry", "probeMain", "-stage", "compute",
            "-o", path.join(outdir, "DxilProbe.dxil")}, nil, true)
        os.tryrm(probe)
        os.tryrm(path.join(outdir, "DxilProbe.dxil"))
        if not dxil_available then
            local reason = probe_err:match("[^\n]*failed to load[^\n]*") or probe_err:match("[^\n]+") or "unknown"
            if get_config("rojorhi_backend") == "d3d12" then
                raise("slang: the DXIL probe compile failed (%s); a d3d12 build requires DXIL; run xmake setup, "
                      .. "which places the pinned DXC beside slangc", reason)
            end
            cprint("${yellow}slang: DXIL skipped, the probe compile failed (%s)", reason)
        else
            cprint("${green}slang: DXIL enabled (dxc probe ok)")
        end
        -- The header lists the entry files and whether their shader libraries exist, written once the DXIL
        -- probe has decided; rewritten only when it changes, so a no-op build does not recompile the test.
        local lines = {"// Generated by rojorhi.slang; do not edit.", "#pragma once", "",
                       "#include <string_view>", "",
                       "inline constexpr std::string_view kConformanceShaders[] = {"}
        for _, name in ipairs(names) do
            table.insert(lines, format("    \"%s\",", name))
        end
        table.insert(lines, "};")
        table.insert(lines, "")
        table.insert(lines, format("inline constexpr bool kMetalLibrariesBuilt = %s;", has_metal and "true" or "false"))
        table.insert(lines, format("inline constexpr bool kD3D12LibrariesBuilt = %s;",
                                   dxil_available and "true" or "false"))
        local header = path.join(config.builddir(), "rojorhi", target:name(), "ShaderList.h")
        local content = table.concat(lines, "\n") .. "\n"
        if not os.isfile(header) or io.readfile(header) ~= content then
            os.mkdir(path.directory(header))
            io.writefile(header, content)
        end

        local module, tool
        if has_metal or dxil_available then
            module = path.join(moduledir, "RojoRHI.slang")
            assert(os.isfile(module), format("rojorhi.slang: %s is missing", module))
            tool = path.join(root, "Tools", "shader_library.py")
            assert(os.isfile(tool), format("rojorhi.slang: %s is missing", tool))
        end
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local entries = entry_points(source)
            local base = {source, "-I", moduledir}
            cprint("${color.build.object}slang %s", source)

            -- The reflection gives the shader library its entry names and stages. Slang renames an entry
            -- named `main` in MSL and warns with E40100 while the reflection keeps `main`, so that warning
            -- is an error: every target keeps the source name.
            local msl = path.join(outdir, name .. ".metal")
            local reflection = path.join(outdir, name .. ".metal.json")
            if stale(msl) or stale(reflection) then
                clear(msl, reflection)
                run_tool(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_METAL", "-target", "metal",
                    "-warnings-as-errors", "40100", "-reflection-json", reflection, "-o", msl}))
            end
            -- The SPIR-V module gives the shader library each entry's threadgroup size, by entry name; without
            -- the flag a module with one entry names it `main`.
            local spv = path.join(outdir, name .. ".spv")
            if stale(spv) then
                clear(spv)
                run_tool(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_SPIRV", "-target", "spirv",
                    "-fvk-use-scalar-layout", "-fvk-use-entrypoint-name", "-o", spv}))
            end
            -- HLSL has no single-output form for several entries, so its text is the compiler's
            -- standard output, which holds every entry of the file.
            local hlsl = path.join(outdir, name .. ".hlsl")
            if stale(hlsl) then
                run_tool(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_HLSL", "-target", "hlsl",
                    "-profile", "sm_6_6"}), hlsl)
            end

            if has_metal then
                -- ADR 0010's floor is macOS 26; without the flag the AIR targets the host SDK's version.
                local lib = path.join(outdir, name .. ".metallib")
                if stale(lib) then
                    clear(lib)
                    run_tool("xcrun", "metal", {"-sdk", "macosx", "metal", "-std=metal4.0",
                        "-mmacosx-version-min=26.0", "-o", lib, msl})
                end
                local library = path.join(outdir, name .. ".metal4lib")
                local inputs = math.max(newest, os.mtime(lib), os.mtime(reflection), os.mtime(spv), os.mtime(tool))
                if not os.isfile(library) or os.mtime(library) < inputs then
                    run_tool("python3", "shader_library", {tool, "--module", module, "--reflection", reflection,
                        "--spirv", spv, "--metallib", lib, "--out", library})
                end
            end

            -- DXIL needs an entry and stage per output; a failure here is a real error. Each entry's DXIL
            -- then goes into the shader library, in the order of the reflection.
            local dxil_args, dxil_files = {}, {}
            for _, entry in ipairs(entries) do
                local dxil = path.join(outdir, #entries == 1 and (name .. ".dxil")
                                       or format("%s.%s.dxil", name, entry.name))
                if dxil_available and stale(dxil) then
                    clear(dxil)
                    run_tool(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_HLSL", "-target", "dxil",
                        "-profile", "sm_6_6", "-entry", entry.name, "-stage", entry.stage, "-o", dxil}))
                end
                table.insert(dxil_args, "--dxil")
                table.insert(dxil_args, entry.name .. "=" .. dxil)
                table.insert(dxil_files, dxil)
            end
            if dxil_available then
                local library = path.join(outdir, name .. ".d3d12lib")
                local inputs = math.max(newest, os.mtime(reflection), os.mtime(spv), os.mtime(tool))
                for _, dxil in ipairs(dxil_files) do
                    inputs = math.max(inputs, os.mtime(dxil))
                end
                if not os.isfile(library) or os.mtime(library) < inputs then
                    run_tool("python3", "shader_library", table.join({tool, "--format", "d3d12", "--module", module,
                        "--reflection", reflection, "--spirv", spv}, dxil_args, {"--out", library}))
                end
            end
        end
    end)


-- Compares the layout of C++ structs with what Slang reflects for a probe shader on every target, and fails
-- the build on drift. A target opts in with add_rules("rojorhi.shader_layout") and supplies, besides the
-- two values rojorhi.slang reads (slang.slangc, slang.moduledir):
--
--   slang.layout_probe    the probe .slang file: StructuredBuffer<T> parameters named gRecord_<Name> and
--                         gRoot_<Name> (Tools/shader_layout.py documents them). rojorhi.slang leaves a
--                         probe out of the conformance shaders when the target also uses that rule.
--   slang.layout_map      optional "Name=Type" entries for names that are not rojoRHI::<Name>
--   slang.layout_include  optional headers, written as in `#include <...>`, that declare the C++ types
--
-- The generator writes $(builddir)/rojorhi/<target>/ShaderLayout.h, a header of static_asserts for the target's
-- C++ to include, and exits non-zero on a root that breaks the flat root rules.
rule("rojorhi.shader_layout")
    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi", target:name()))
    end)

    before_build(function (target)
        import("core.project.config")
        local moduledir = assert(target:values("slang.moduledir"),
                                 "rojorhi.shader_layout: set_values(\"slang.moduledir\", <dir>)")
        local slangc = assert(target:values("slang.slangc"),
                              "rojorhi.shader_layout: set_values(\"slang.slangc\", <slangc>)")
        local probe = assert(target:values("slang.layout_probe"),
                             "rojorhi.shader_layout: set_values(\"slang.layout_probe\", <file>)")
        assert(os.isfile(slangc), format("rojorhi.shader_layout: %s is missing; run `xmake setup`", slangc))
        local tool = path.join(path.directory(path.directory(moduledir)), "Tools", "shader_layout.py")
        assert(os.isfile(tool), format("rojorhi.shader_layout: %s is missing", tool))

        local outdir = path.join(config.builddir(), "rojorhi", target:name())
        local header = path.join(outdir, "ShaderLayout.h")
        local newest = math.max(os.mtime(slangc), os.mtime(tool), os.mtime(probe))
        for _, file in ipairs(os.files(path.join(moduledir, "**.slang"))) do
            newest = math.max(newest, os.mtime(file))
        end
        if os.isfile(header) and os.mtime(header) >= newest then
            return
        end

        -- One compile per target, each with its own macro, profile and flags. SPIR-V takes no -profile, as in
        -- rojorhi.slang, so both read the one default. The compiled output is discarded; only the reflection
        -- is read.
        local targets = {
            {name = "metal", define = "ROJORHI_TARGET_METAL", profile = nil},
            {name = "spirv", define = "ROJORHI_TARGET_SPIRV", profile = nil,
             flags = {"-fvk-use-scalar-layout"}},
            {name = "hlsl", define = "ROJORHI_TARGET_HLSL", profile = "sm_6_6"},
        }
        cprint("${color.build.object}slang layout %s", probe)
        os.mkdir(outdir)
        local inputs = {}
        for _, t in ipairs(targets) do
            local json = path.join(outdir, "layout." .. t.name .. ".json")
            local args = {probe, "-I", moduledir, "-D", t.define, "-target", t.name,
                          "-entry", "probeMain", "-stage", "compute",
                          "-o", path.join(outdir, "layout." .. t.name .. ".out"), "-reflection-json", json}
            if t.profile then
                table.insert(args, "-profile")
                table.insert(args, t.profile)
            end
            -- `load<T>` dereferences natural layout, which SPIR-V names scalar layout; without the flag the
            -- reflection reports std430.
            table.join2(args, t.flags or {})
            local errfile = os.tmpfile()
            local ok = try {function () os.execv(slangc, args, {stderr = errfile}); return true end}
            local err = os.isfile(errfile) and io.readfile(errfile) or ""
            os.tryrm(errfile)
            if not ok then
                os.raise("rojorhi.shader_layout: slangc failed for %s\n%s", table.concat(args, " "), err)
            end
            table.insert(inputs, t.name .. "=" .. json)
        end

        local args = table.join({tool}, inputs, {"--out", header})
        for _, entry in ipairs(table.wrap(target:values("slang.layout_map"))) do
            table.join2(args, {"--map", entry})
        end
        for _, include in ipairs(table.wrap(target:values("slang.layout_include"))) do
            table.join2(args, {"--include", include})
        end
        os.runv("python3", args)
    end)
