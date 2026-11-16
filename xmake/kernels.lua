-- The backends' own kernels, compiled at build time and embedded in the library as bytes. A target opts in with one rule and
-- its own add_files; only the backend the rule serves does, so another build never reads the kernels and needs none of the
-- toolchain. Two rules live here:
--
--   rojorhi.metal4_kernels  MSL -> AIR -> one .metallib for the Metal 4 backend, from add_files("<dir>/*.metal")
--   rojorhi.d3d12_kernels   HLSL -> one signed DXIL container per file for the D3D12 backend, from add_files("<dir>/*.hlsl")
--
-- rojorhi.metal4_kernels compiles each .metal file to <builddir>/rojorhi/Metal4Kernels/<name>.air, links the AIR files into
-- Metal4Kernels.metallib there, and writes its bytes to <builddir>/rojorhi/Metal4KernelLibrary.h as
-- `rojoRHI::metal4::kKernelLibrary`. Without a Metal toolchain the build fails: the backend cannot run without its kernels.
--
-- rojorhi.d3d12_kernels compiles each .hlsl file with the DXC the target value `rojorhi.dxc` names (an absolute path, since
-- the project root differs between this component and a host that mounts it) as a cs_6_6 kernel with entry `main`, HLSL 2021
-- and warnings as errors, to <builddir>/rojorhi/D3D12Kernels/<name>.dxil. DXC signs the container through the dxil.dll beside
-- it. The bytes of each file go to <builddir>/rojorhi/D3D12KernelLibrary.h as `rojoRHI::d3d12::k<name>Kernel`. A missing DXC
-- fails the build, naming `xmake setup`, which fetches the pinned one. A DXC newer than a .dxil, as a new pin brings, compiles
-- that kernel again.
--
-- In both rules the flags do not depend on the build mode, because Debug and Release share these outputs. A header is
-- rewritten only when its content changes, so an unchanged kernel recompiles no C++. The work runs in before_build so the
-- header exists before the target's C++ compiles. Each step is skipped while its output is newer than its inputs and this
-- file, and the toolchain is looked for only when a step has work to do.

-- Script scope does not see this file's locals, so the helpers live inside the callback.
rule("rojorhi.metal4_kernels")
    set_extensions(".metal")

    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi"))
    end)

    before_build(function (target)
        import("core.project.config")
        local batch = target:sourcebatches()["rojorhi.metal4_kernels"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)
        if #sources == 0 then
            os.raise("rojorhi.metal4_kernels: target '%s' has no .metal kernel files", target:name())
        end

        local rulefile = path.join(os.scriptdir(), "kernels.lua")
        local outdir = path.join(config.builddir(), "rojorhi", "Metal4Kernels")
        local library = path.join(outdir, "Metal4Kernels.metallib")
        local header = path.join(config.builddir(), "rojorhi", "Metal4KernelLibrary.h")
        os.mkdir(outdir)

        -- The toolchain is probed once, and only when a step has work to do.
        local probed = false
        local function require_toolchain()
            if probed then
                return
            end
            local found = try {function ()
                os.iorunv("xcrun", {"metal", "--version"}); return true
            end}
            if not found then
                os.raise("rojorhi.metal4_kernels: `xcrun metal --version` found no Metal toolchain, which the metal4 backend needs "
                         .. "to compile its kernels; install it with `xcodebuild -downloadComponent MetalToolchain`")
            end
            probed = true
        end
        local function stale(output, inputs)
            if not os.isfile(output) then
                return true
            end
            local mtime = os.mtime(output)
            for _, input in ipairs(inputs) do
                if os.mtime(input) > mtime then
                    return true
                end
            end
            return false
        end

        -- The list of kernel files is an input of the link, so adding or removing one relinks the library.
        local list = path.join(outdir, "Metal4Kernels.list")
        local names = table.concat(sources, "\n") .. "\n"
        if not os.isfile(list) or io.readfile(list) ~= names then
            io.writefile(list, names)
        end

        local airs = {}
        for _, source in ipairs(sources) do
            local air = path.join(outdir, path.basename(source) .. ".air")
            if stale(air, {source, rulefile}) then
                require_toolchain()
                cprint("${color.build.object}metal %s", source)
                os.vrunv("xcrun", {"-sdk", "macosx", "metal", "-std=metal4.0", "-c", source, "-o", air})
            end
            table.insert(airs, air)
        end
        if stale(library, table.join(airs, {list, rulefile})) then
            require_toolchain()
            cprint("${color.build.object}metallib %s", library)
            os.vrunv("xcrun", table.join({"-sdk", "macosx", "metallib"}, airs, {"-o", library}))
        end

        if stale(header, {library, rulefile}) then
            local bytes = io.readfile(library, {encoding = "binary"})
            local lines = {"// Generated by rojorhi.metal4_kernels from the Metal 4 backend's kernels; do not edit.",
                           "#pragma once", "", "namespace rojoRHI::metal4 {",
                           "    /// The backend's kernel library, a metallib of every kernel.",
                           "    inline constexpr unsigned char kKernelLibrary[] = {"}
            local row = {}
            for i = 1, #bytes do
                table.insert(row, format("0x%02x,", bytes:byte(i)))
                if #row == 24 or i == #bytes then
                    table.insert(lines, "        " .. table.concat(row, " "))
                    row = {}
                end
            end
            table.insert(lines, "    };")
            table.insert(lines, "} // namespace rojoRHI::metal4")
            local content = table.concat(lines, "\n") .. "\n"
            -- Unchanged bytes leave the header and its time alone, which would otherwise recompile its includer; the
            -- comparison then repeats on each build until the library changes again, a read of a few kilobytes.
            if not os.isfile(header) or io.readfile(header) ~= content then
                io.writefile(header, content)
            end
        end
    end)

-- Script scope does not see this file's locals, so the helpers live inside the callback.
rule("rojorhi.d3d12_kernels")
    set_extensions(".hlsl")

    on_load(function (target)
        target:add("includedirs", path.join("$(builddir)", "rojorhi"))
    end)

    before_build(function (target)
        import("core.project.config")
        local batch = target:sourcebatches()["rojorhi.d3d12_kernels"]
        local sources = table.unique(table.wrap(batch and batch.sourcefiles))
        table.sort(sources)
        if #sources == 0 then
            os.raise("rojorhi.d3d12_kernels: target '%s' has no .hlsl kernel files", target:name())
        end
        local dxc = target:values("rojorhi.dxc")
        if not dxc then
            os.raise("rojorhi.d3d12_kernels: target '%s' sets no rojorhi.dxc, the path of the DXC that compiles its kernels",
                     target:name())
        end

        local rulefile = path.join(os.scriptdir(), "kernels.lua")
        local outdir = path.join(config.builddir(), "rojorhi", "D3D12Kernels")
        local header = path.join(config.builddir(), "rojorhi", "D3D12KernelLibrary.h")
        os.mkdir(outdir)

        local function require_dxc()
            if not os.isfile(dxc) then
                os.raise("rojorhi.d3d12_kernels: %s is missing, which the d3d12 backend needs to compile its kernels; run "
                         .. "`xmake setup`", dxc)
            end
        end
        local function stale(output, inputs)
            if not os.isfile(output) then
                return true
            end
            local mtime = os.mtime(output)
            for _, input in ipairs(inputs) do
                if os.mtime(input) > mtime then
                    return true
                end
            end
            return false
        end

        -- The list of kernel files is an input of the header, so adding or removing one rewrites it.
        local list = path.join(outdir, "D3D12Kernels.list")
        local names = table.concat(sources, "\n") .. "\n"
        if not os.isfile(list) or io.readfile(list) ~= names then
            io.writefile(list, names)
        end

        local kernels = {}
        for _, source in ipairs(sources) do
            local name = path.basename(source)
            if not name:match("^[%a_][%w_]*$") then
                os.raise("rojorhi.d3d12_kernels: %s does not name a C++ identifier, which its kernel's array is named after",
                         source)
            end
            local dxil = path.join(outdir, name .. ".dxil")
            if stale(dxil, {source, rulefile, dxc}) then
                require_dxc()
                cprint("${color.build.object}dxc %s", source)
                os.vrunv(dxc, {"-T", "cs_6_6", "-E", "main", "-HV", "2021", "-WX", "-Fo", dxil, source})
            end
            table.insert(kernels, {name = name, dxil = dxil})
        end

        local inputs = {list, rulefile}
        for _, kernel in ipairs(kernels) do
            table.insert(inputs, kernel.dxil)
        end
        if stale(header, inputs) then
            local lines = {"// Generated by rojorhi.d3d12_kernels from the D3D12 backend's kernels; do not edit.",
                           "#pragma once", "", "namespace rojoRHI::d3d12 {"}
            for _, kernel in ipairs(kernels) do
                local bytes = io.readfile(kernel.dxil, {encoding = "binary"})
                table.insert(lines, format("    /// The signed DXIL of Kernels/%s.hlsl.", kernel.name))
                table.insert(lines, format("    inline constexpr unsigned char k%sKernel[] = {", kernel.name))
                local row = {}
                for i = 1, #bytes do
                    table.insert(row, format("0x%02x,", bytes:byte(i)))
                    if #row == 24 or i == #bytes then
                        table.insert(lines, "        " .. table.concat(row, " "))
                        row = {}
                    end
                end
                table.insert(lines, "    };")
            end
            table.insert(lines, "} // namespace rojoRHI::d3d12")
            local content = table.concat(lines, "\n") .. "\n"
            -- Unchanged bytes leave the header and its time alone, which would otherwise recompile its includer.
            if not os.isfile(header) or io.readfile(header) ~= content then
                io.writefile(header, content)
            end
        end
    end)
