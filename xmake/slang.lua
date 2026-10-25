-- Slang -> MSL, SPIR-V, HLSL, a Null shader library and, where the host can, .metallib and DXIL for the shader
-- entry files of a target in the new interface tree. This comment is the rule's consumer interface. A target opts
-- in with add_rules("rojorhi.slang") plus add_files(<entry files>), and sets these values. Every path value is
-- truly absolute, because the project root differs between this component's own root and a host that mounts it,
-- and target:values() expands no `$(projectdir)` or other variable: build each path from os.scriptdir(), as
-- path.join(os.scriptdir(), "Shaders/Modules").
--
--   slang.slangc         required: the pinned Slang compiler (slangc.exe on Windows)
--   slang.moduledir      required: RojoRHI's Shaders/RojoRHI, passed first as -I; its grandparent locates
--                        xmake/slang.lua and Tools/
--   slang.includedirs    optional: directories of shared modules, passed as -I after moduledir in the given
--                        order and scanned for staleness; each must be an existing absolute directory.
--                        rojorhi.shader_layout honours them too
--   slang.embed          optional: a bare header file name, such as "ImGuiShaders.h", never a path; the header
--                        is written into the generated header directory below. Renaming it leaves the header
--                        of the old name there until the build directory is cleaned
--   slang.embed_files    optional: basenames of the entry files to embed; default every entry file
--   slang.precise_files  optional: exact entry-file basenames without extension, as for embed_files;
--                        default empty. Every function in a selected file gets Slang -fp-mode precise
--                        on all four targets, DXIL -Xdxc -Gis and Metal -fno-fast-math -ffp-contract=off. Names are
--                        case-sensitive; unknown names, paths, extensions, duplicates (including case
--                        variants), layout probes and non-strings fail before probes or output changes
--
-- Inputs: every add_files input declares at least one [shader("<stage>")] entry outside comments, or the build
-- fails; shared modules are reached through slang.includedirs, never listed. Outputs are flat, so basenames are
-- unique case-insensitively, and no entry is named `main` (the MSL compile would rename it). Basenames
-- are checked per target only, so two targets sharing a targetdir must not reuse an entry basename.
--
-- Outputs land in <targetdir>/Shaders/, per entry file <name>: <name>.metal, <name>.metal.json (the reflection
-- of the MSL compile), <name>.spv, <name>.hlsl and <name>.nulllib on every host; <name>.metallib and
-- <name>.metal4lib with the Metal toolchain; <name>.dxil (or <name>.<entry>.dxil for a file of several entries)
-- and <name>.d3d12lib with a DXC beside slangc. A d3d12 configure requires DXIL and fails without that DXC.
-- Tools/shader_library.py packages each library from the reflection: the .metal4lib is the metallib behind a
-- text header with the ROJORHI_SHADER_ABI_VERSION stamp and each entry's stage and threadgroup size (read from
-- the SPIR-V module), the .d3d12lib the same around each entry's signed DXIL, and the .nulllib the text header
-- alone, which the Null backend loads. Metallibs target the macOS 26 deployment floor. Requirements: Python 3 on
-- every host (python3, else python, on PATH), the Metal toolchain for metallibs, a DXC for DXIL.
--
-- Generated headers land in $(builddir)/rojorhi/<target>/, which the rule adds to that target's include path
-- only, and are rewritten only when their text changes, so a no-op build recompiles nothing:
--
--   ShaderList.h     `kConformanceShaders`, the entry basenames sorted, for a test to enumerate, with
--                    `kMetalLibrariesBuilt` and `kD3D12LibrariesBuilt` saying whether the .metal4lib and
--                    .d3d12lib files exist, and `kNullLibrariesBuilt`, always true
--   <slang.embed>    namespace rojoRHI::embedded holding `inline constexpr std::byte k<Name>Library[]` for
--                    each embedded entry file: the configured backend's library (metal4 .metal4lib, d3d12
--                    .d3d12lib, null .nulllib), written by Tools/embed_library.py, ready for
--                    Device::loadShaderLibrary. A library the build did not produce fails the build. The
--                    embed step runs on every build, so a backend switch takes effect, and rewrites the
--                    header only when its text changes.
--
-- Any failed compile fails the build with the compiler's own output, and a successful compile's warnings,
-- `metal`'s included, are printed. This rule is separate from the v1 rule of shaders.lua, which Legacy/ keeps.
--
-- Rule "rojorhi.shader_layout", declared below, checks C++ struct layouts against the reflection of a probe.
--
-- The work runs in before_build so the generated headers exist before the target's C++ compiles. Each output is
-- skipped while it is newer than every entry, every module under moduledir and slang.includedirs, the compiler
-- and this rule. A shader library is also rebuilt when its metallib or DXIL, reflection, SPIR-V or the packaging
-- tool is newer. A per-source <name>.compile-policy success record also captures the effective compiler,
-- ordered module/include paths, precision flags and optional-output availability. Missing or changed policy
-- rebuilds all products of that source. Invalidate the record before artifact mutation and write it only after
-- all products succeed, so failures/retries/policy reversals cannot reuse an old success. Initial migration
-- may rebuild ordinary files once; sibling policy changes and no-op builds then preserve their outputs.

-- Script scope does not see this file's locals, so the helpers live inside the callback.
rule("rojorhi.slang")
    set_extensions(".slang")

    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi", target:name()))
    end)

    before_build(function (target)
        import("core.project.config")
        import("core.base.json")
        import("lib.detect.find_program")
        -- Entry points of a source file, read from its [shader("stage")] attributes with comments removed.
        -- Each result is {name = <function>, stage = <stage>}. A file with several attributes compiles every
        -- entry; a file with none is not an entry file and fails the build before any compile.
        local function entry_points(source)
            local text = io.readfile(source):gsub("/%*.-%*/", ""):gsub("//[^\n]*", "")
            local entries = {}
            local init = 1
            while true do
                local first, last, stage = text:find("%[%s*shader%s*%(%s*\"(%w+)\"%s*%)%s*%]", init)
                if not first then break end
                -- After the attribute come further bracketed attributes, then the declaration: the return
                -- type, which may hold spaces (`vector<float, 4>`), and the function name, the identifier
                -- right before the first `(`.
                local tail = text:sub(last + 1)
                local stripped = tail:gsub("^%s*%b[]", "")
                while stripped ~= tail do
                    tail = stripped
                    stripped = tail:gsub("^%s*%b[]", "")
                end
                local declaration = tail:match("^([^(;{}]*)%(")
                local name = declaration and declaration:match("%S%s+([%a_][%w_]*)%s*$")
                if not name then
                    os.raise("rojorhi.slang: no function declaration (return type, name, `(`) follows the "
                             .. "[shader(\"%s\")] attribute in %s", stage, source)
                end
                table.insert(entries, {name = name, stage = stage})
                init = last + 1
            end
            return entries
        end

        -- Runs `program` (slangc, xcrun or Python) and raises with its own output, under `label`, when it
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
        local includedirs = table.wrap(target:values("slang.includedirs"))
        for _, dir in ipairs(includedirs) do
            if not path.is_absolute(dir) or not os.isdir(dir) then
                os.raise("rojorhi.slang: slang.includedirs entry '%s' is not an existing absolute directory", dir)
            end
        end

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

        -- Every input is an entry file; a shared module listed by mistake would compile to empty outputs.
        local entries_of = {}
        for _, source in ipairs(sources) do
            entries_of[source] = entry_points(source)
            if #entries_of[source] == 0 then
                os.raise("rojorhi.slang: %s declares no [shader(\"...\")] entry point; pass only entry files to "
                         .. "add_files and reach shared modules through slang.includedirs", source)
            end
        end

        -- Precision is owned by an entry file, not a function or a naming convention. Validate before
        -- any tool probe or output mutation, including the Python and optional-output probes below.
        local precise_files, seen = {}, {}
        local known = {}
        for _, name in ipairs(names) do known[name] = true end
        -- xmake deduplicates values.<key> before callbacks. Its original values dictionary preserves
        -- repetitions from set_values/add_values, so inspect it as well instead of silently accepting them.
        local declared = target:get("values") or {}
        local selection = declared["slang.precise_files"]
        if selection == nil then selection = target:values("slang.precise_files") end
        for _, name in ipairs(table.wrap(selection)) do
            if type(name) ~= "string" then
                os.raise("rojorhi.slang: target '%s' slang.precise_files value '%s' must be a string entry-file basename",
                         target:name(), tostring(name))
            end
            local key = name:lower()
            if seen[key] then
                os.raise("rojorhi.slang: target '%s' slang.precise_files repeats basename '%s' (already '%s')",
                         target:name(), name, seen[key])
            end
            seen[key] = name
            if not known[name] then
                os.raise("rojorhi.slang: target '%s' slang.precise_files names '%s'; expected an exact entry-file "
                         .. "basename without extension (case-sensitive, no paths or layout probes)", target:name(), name)
            end
            precise_files[name] = true
        end

        -- Python 3 runs the packaging tools. Windows installs from python.org ship python.exe and no
        -- python3.exe, so `python` is the fallback; the check rejects a Python 2 and the Windows Store stub.
        local python_check = {"-c", "import sys; sys.exit(sys.version_info[0] < 3)"}
        local python = find_program("python3", {check = python_check})
                       or find_program("python", {check = python_check})
        if not python then
            os.raise("rojorhi.slang: no Python 3 found as python3 or python on PATH")
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

        -- Anything a compile reads: the entries, every module under each -I directory, the compiler, and this rule
        -- for its flags. The rule sits in xmake/ beside Shaders/, as Tools/ does.
        local root = path.directory(path.directory(moduledir))
        local rule_file = path.join(root, "xmake", "slang.lua")
        assert(os.isfile(rule_file), format("rojorhi.slang: %s is missing", rule_file))
        local newest = math.max(os.mtime(slangc), os.mtime(rule_file))
        local modules = os.files(path.join(moduledir, "**.slang"))
        for _, dir in ipairs(includedirs) do
            table.join2(modules, os.files(path.join(dir, "**.slang")))
        end
        for _, file in ipairs(table.join(sources, modules)) do
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
                -- A fresh old product surviving cleanup must never certify a new policy. Check every
                -- output, including containers and the success record, before any generator can skip it.
                assert(not os.exists(output), format("rojorhi.slang: cannot remove stale shader output %s", output))
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
        table.insert(lines, "inline constexpr bool kNullLibrariesBuilt = true;")
        local header = path.join(config.builddir(), "rojorhi", target:name(), "ShaderList.h")
        local content = table.concat(lines, "\n") .. "\n"
        if not os.isfile(header) or io.readfile(header) ~= content then
            os.mkdir(path.directory(header))
            io.writefile(header, content)
        end

        -- Every host packages a Null library, so the module (for the ABI stamp) and the tool are always needed.
        local module = path.join(moduledir, "RojoRHI.slang")
        assert(os.isfile(module), format("rojorhi.slang: %s is missing", module))
        local tool = path.join(root, "Tools", "shader_library.py")
        assert(os.isfile(tool), format("rojorhi.slang: %s is missing", tool))
        local include_args = {"-I", moduledir}
        for _, dir in ipairs(includedirs) do
            table.join2(include_args, {"-I", dir})
        end
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            local entries = entries_of[source]
            local precision_args = precise_files[name] and {"-fp-mode", "precise"} or {}
            local metal_args = precise_files[name] and {"-fno-fast-math", "-ffp-contract=off"} or {}
            local downstream_args = precise_files[name] and {"-Xdxc", "-Gis"} or {}
            local base = table.join({source}, include_args, precision_args)
            -- Ordered arrays give a canonical effective policy: a sibling's selection never appears here.
            -- This is a success record, not a timestamp dependency. Its absence also makes a partial
            -- failure stale when the user reverts to the policy that succeeded before that failure.
            local effective_policy = {1, path.absolute(source), path.absolute(slangc), path.absolute(moduledir),
                                      includedirs, precision_args, metal_args, has_metal and true or false,
                                      dxil_available}
            -- Preserve ordinary records byte-for-byte; only selected sources acquire the downstream policy.
            if precise_files[name] then table.insert(effective_policy, downstream_args) end
            local policy = json.encode(effective_policy)
            local stamp = path.join(outdir, name .. ".compile-policy")
            local changed_policy = not os.isfile(stamp) or io.readfile(stamp) ~= policy
            local products = {}
            for _, ext in ipairs({"metal", "metal.json", "spv", "hlsl", "nulllib", "metallib", "metal4lib",
                                  "dxil", "d3d12lib"}) do
                table.insert(products, path.join(outdir, name .. "." .. ext))
            end
            for _, entry in ipairs(entries) do
                if #entries > 1 then
                    table.insert(products, path.join(outdir, format("%s.%s.dxil", name, entry.name)))
                end
            end
            local required = {path.join(outdir, name .. ".metal"), path.join(outdir, name .. ".metal.json"),
                              path.join(outdir, name .. ".spv"), path.join(outdir, name .. ".hlsl"),
                              path.join(outdir, name .. ".nulllib")}
            if has_metal then
                table.join2(required, {path.join(outdir, name .. ".metallib"), path.join(outdir, name .. ".metal4lib")})
            end
            if dxil_available then
                table.insert(required, path.join(outdir, name .. ".d3d12lib"))
                for _, entry in ipairs(entries) do
                    table.insert(required, path.join(outdir, #entries == 1 and (name .. ".dxil")
                                           or format("%s.%s.dxil", name, entry.name)))
                end
            end
            local rebuild = changed_policy
            for _, output in ipairs(required) do
                if stale(output) or os.mtime(output) < os.mtime(tool) then rebuild = true end
            end
            local function older_than(output, inputs)
                if not os.isfile(output) then return true end
                for _, input in ipairs(inputs) do
                    if os.isfile(input) and os.mtime(output) < os.mtime(input) then return true end
                end
                return false
            end
            local reflection_file = path.join(outdir, name .. ".metal.json")
            local spirv_file = path.join(outdir, name .. ".spv")
            if older_than(path.join(outdir, name .. ".nulllib"), {reflection_file, tool}) then rebuild = true end
            if has_metal and older_than(path.join(outdir, name .. ".metal4lib"),
                                       {path.join(outdir, name .. ".metallib"), reflection_file, spirv_file, tool}) then
                rebuild = true
            end
            if dxil_available then
                local inputs = {reflection_file, spirv_file, tool}
                for _, entry in ipairs(entries) do
                    table.insert(inputs, path.join(outdir, #entries == 1 and (name .. ".dxil")
                                          or format("%s.%s.dxil", name, entry.name)))
                end
                if older_than(path.join(outdir, name .. ".d3d12lib"), inputs) then rebuild = true end
            end
            if rebuild then
                -- Invalidate BEFORE touching any artifact. Rebuild the whole source transaction so its
                -- reflection and every container always describe the same successful compilation policy.
                clear(stamp)
                for _, output in ipairs(products) do clear(output) end
            end
            -- The source is named once, before its first stale output is rebuilt, so a no-op build prints none.
            local announced = false
            local function run(...)
                if not announced then
                    cprint("${color.build.object}slang %s", source)
                    announced = true
                end
                return run_tool(...)
            end

            -- The reflection gives the shader library its entry names and stages. Slang renames an entry
            -- named `main` in MSL and warns with E40100 while the reflection keeps `main`, so that warning
            -- is an error: every target keeps the source name.
            local msl = path.join(outdir, name .. ".metal")
            local reflection = path.join(outdir, name .. ".metal.json")
            if stale(msl) or stale(reflection) then
                clear(msl, reflection)
                run(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_METAL", "-target", "metal",
                    "-warnings-as-errors", "40100", "-reflection-json", reflection, "-o", msl}))
            end
            -- The SPIR-V module gives the shader library each entry's threadgroup size, by entry name; without
            -- the flag a module with one entry names it `main`.
            local spv = path.join(outdir, name .. ".spv")
            if stale(spv) then
                clear(spv)
                run(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_SPIRV", "-target", "spirv",
                    "-fvk-use-scalar-layout", "-fvk-use-entrypoint-name", "-o", spv}))
            end
            -- HLSL has no single-output form for several entries, so its text is the compiler's
            -- standard output, which holds every entry of the file.
            local hlsl = path.join(outdir, name .. ".hlsl")
            if stale(hlsl) then
                run(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_HLSL", "-target", "hlsl",
                    "-profile", "sm_6_6"}), hlsl)
            end
            -- The Null library lists the reflection's entries, in the order the other two libraries use.
            local nulllib = path.join(outdir, name .. ".nulllib")
            if not os.isfile(nulllib) or os.mtime(nulllib) < math.max(newest, os.mtime(reflection), os.mtime(tool)) then
                run(python, "shader_library", {tool, "--format", "null", "--module", module,
                    "--reflection", reflection, "--out", nulllib})
            end

            if has_metal then
                -- ADR 0010's floor is macOS 26; without the flag the AIR targets the host SDK's version.
                local lib = path.join(outdir, name .. ".metallib")
                if stale(lib) then
                    clear(lib)
                    run("xcrun", "metal", table.join({"-sdk", "macosx", "metal", "-std=metal4.0",
                        "-mmacosx-version-min=26.0"}, metal_args, {"-o", lib, msl}))
                end
                local library = path.join(outdir, name .. ".metal4lib")
                local inputs = math.max(newest, os.mtime(lib), os.mtime(reflection), os.mtime(spv), os.mtime(tool))
                if not os.isfile(library) or os.mtime(library) < inputs then
                    run(python, "shader_library", {tool, "--module", module, "--reflection", reflection,
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
                    run(slangc, "slangc", table.join(base, {"-D", "ROJORHI_TARGET_HLSL", "-target", "dxil",
                        "-profile", "sm_6_6", "-entry", entry.name, "-stage", entry.stage, "-o", dxil},
                        downstream_args))
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
                    run(python, "shader_library", table.join({tool, "--format", "d3d12", "--module", module,
                        "--reflection", reflection, "--spirv", spv}, dxil_args, {"--out", library}))
                end
            end
            if rebuild then
                for _, output in ipairs(required) do
                    assert(os.isfile(output), format("rojorhi.slang: successful compile did not produce %s", output))
                end
                io.writefile(stamp, policy)
            end
        end

        -- The embed header holds the configured backend's library of each selected entry file. The tool runs on
        -- every build and writes only when the text changes, so a backend switch rewrites it and a no-op build
        -- does not.
        local embed = target:values("slang.embed")
        if not embed and target:values("slang.embed_files") then
            os.raise("rojorhi.slang: target '%s' sets slang.embed_files without slang.embed", target:name())
        end
        if embed then
            embed = table.concat(table.wrap(embed), " ")
            if embed ~= path.filename(embed) or not embed:endswith(".h") or embed:find("%s") then
                os.raise("rojorhi.slang: slang.embed '%s' must be one bare header file name ending in .h", embed)
            end
            local selected = table.wrap(target:values("slang.embed_files"))
            if #selected == 0 then
                selected = names
            end
            local known = {}
            for _, name in ipairs(names) do
                known[name] = true
            end
            local backend = get_config("rojorhi_backend")
            local embed_format = (backend == "metal4" or backend == "d3d12") and backend or "null"
            local extension = "." .. embed_format .. "lib"
            local embed_tool = path.join(root, "Tools", "embed_library.py")
            assert(os.isfile(embed_tool), format("rojorhi.slang: %s is missing", embed_tool))
            local args = {embed_tool, "--out", path.join(config.builddir(), "rojorhi", target:name(), embed),
                          "--format", embed_format}
            for _, name in ipairs(selected) do
                if not known[name] then
                    os.raise("rojorhi.slang: slang.embed_files names '%s', which is not an entry file of target '%s'",
                             name, target:name())
                end
                local library = path.join(outdir, name .. extension)
                if not os.isfile(library) then
                    if embed_format == "metal4" then
                        os.raise("rojorhi.slang: slang.embed needs %s, which the Metal toolchain builds", library)
                    end
                    os.raise("rojorhi.slang: slang.embed needs %s, which this build did not produce", library)
                end
                table.join2(args, {"--library", name .. "=" .. library})
            end
            run_tool(python, "embed_library", args)
        end
    end)


-- Compares the layout of C++ structs with what Slang reflects for a probe shader on every target, and fails
-- the build on drift. A target opts in with add_rules("rojorhi.shader_layout") and supplies, besides the
-- two values rojorhi.slang reads (slang.slangc, slang.moduledir) and its optional slang.includedirs, passed as -I
-- after moduledir and scanned for staleness:
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
        import("lib.detect.find_program")
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
        local includedirs = table.wrap(target:values("slang.includedirs"))
        local include_args = {"-I", moduledir}
        for _, dir in ipairs(includedirs) do
            if not path.is_absolute(dir) or not os.isdir(dir) then
                os.raise("rojorhi.shader_layout: slang.includedirs entry '%s' is not an existing absolute directory", dir)
            end
            table.join2(include_args, {"-I", dir})
        end
        local newest = math.max(os.mtime(slangc), os.mtime(tool), os.mtime(probe))
        for _, dir in ipairs(table.join({moduledir}, includedirs)) do
            for _, file in ipairs(os.files(path.join(dir, "**.slang"))) do
                newest = math.max(newest, os.mtime(file))
            end
        end
        -- The generator rewrites the header only when its text changes, so the stamp, written after each run,
        -- records when the check last ran; without it an input edit that changes no layout reruns the check on
        -- every later build.
        local stamp = path.join(outdir, "ShaderLayout.stamp")
        if os.isfile(header) and os.isfile(stamp) and os.mtime(stamp) >= newest then
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
            local args = table.join({probe}, include_args, {"-D", t.define, "-target", t.name,
                          "-entry", "probeMain", "-stage", "compute",
                          "-o", path.join(outdir, "layout." .. t.name .. ".out"), "-reflection-json", json})
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
        -- As in rojorhi.slang: python3, else a Python 3 named python, as python.org installs it on Windows.
        local python_check = {"-c", "import sys; sys.exit(sys.version_info[0] < 3)"}
        local python = find_program("python3", {check = python_check})
                       or find_program("python", {check = python_check})
        if not python then
            os.raise("rojorhi.shader_layout: no Python 3 found as python3 or python on PATH")
        end
        os.runv(python, args)
        io.writefile(stamp, "")
    end)
