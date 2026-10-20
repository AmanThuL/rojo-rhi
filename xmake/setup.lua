-- Bootstrap for a standalone configure of this component: the vendored dependencies its targets
-- compile and run against, at the same pins a host project uses. A host has a setup task of its
-- own and never includes this file, so exactly one `setup` task exists per project root.
--
-- Everything lands in <project root>/ThirdParty, which is the directory the targets default to
-- when no host overrides it.
local metalcpp_pin = "release/metal-cpp_macOS26.4_iOS26.4"
local metalcpp_commit = "c595afef4a5dc388f4047cd0c69f9e7f9468d9ed"
-- Every downloaded archive is checked by its SHA-256 before anything is extracted from it, and the
-- Slang compiler binary is checked again by its own hash after extraction. metal-cpp is pinned by
-- commit, which git verifies.
local slang_pin    = "v2026.14.1"
local slang_sha256 = "a1c5ecae0d2425b13fe7f616686f2df7cc7028d3f6a85fb717497cf98bee3d0a"
local slang_macos_archive_sha256 = "2976c3a9a6f4d77b5734d00b5d841d1ff087d9965d9006b9b4d73edd0062cb7d"
-- The Windows archive's compiler, checked by the hash of bin/slangc.exe.
local slang_windows_sha256 = "b9f786a651569aa4f968e4014d04b6e2f4f1a6c9584f47ea9f4ccc841fdeeeeb"
local slang_windows_archive_sha256 = "5ed0a59d650a0af0aca45d5db4e083b3d8fb5cea05748747dd95dfbe9c580658"
-- The shader compiler Slang loads to produce DXIL on Windows, placed beside slangc.exe.
local dxc_pin    = "v1.9.2607"
local dxc_archive = "dxc_2026_07_29.zip"
local dxc_sha256 = "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241"
-- The D3D12 runtime a Windows executable loads from beside itself: the Agility SDK (its headers and
-- D3D12Core.dll with the debug layer) and WARP, the software adapter hosted CI runs on. Each pin is
-- the SHA-256 of the NuGet package, checked before anything is extracted from it.
local agility_pin    = "1.619.6"
local agility_sha256 = "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9"
local warp_pin       = "1.0.21"
local warp_sha256    = "ea44b77eb30eec14427193e20f40cdb2e4c31ed11ab39cb880538fdf4bac2681"
-- The FidelityFX SDK's upscaler runtime a D3D12 executable loads from beside itself, and the API headers that declare
-- it: raw files of the tag (commit 60f4ea81909200d8542eca14dccb2628b763a9a3), each checked by the SHA-256 of the file
-- downloaded. The headers keep their layout under Kits/FidelityFX, which ffx_upscale.h's relative includes need; the
-- DLLs are the AMD-signed ones of signedbin/, under the binary licence in license.md.
local fidelityfx_pin = "v2.3.0"
local fidelityfx_files = {
    {from = "Kits/FidelityFX/api/include/ffx_api.h", to = "include/api/include/ffx_api.h",
     sha256 = "e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd"},
    {from = "Kits/FidelityFX/api/include/ffx_api.hpp", to = "include/api/include/ffx_api.hpp",
     sha256 = "e1fab07fa9ab489db74813e6b754fc91dad6ded68870ab558002e92b6a32566a"},
    {from = "Kits/FidelityFX/api/include/ffx_api_loader.h", to = "include/api/include/ffx_api_loader.h",
     sha256 = "18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"},
    {from = "Kits/FidelityFX/api/include/ffx_api_types.h", to = "include/api/include/ffx_api_types.h",
     sha256 = "cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49"},
    {from = "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h", to = "include/api/include/dx12/ffx_api_dx12.h",
     sha256 = "c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"},
    {from = "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.hpp", to = "include/api/include/dx12/ffx_api_dx12.hpp",
     sha256 = "31972deed9c5b94243eb72c97e24c1b4e1a0171ae0d0109f7fe842b6efdeddc7"},
    {from = "Kits/FidelityFX/upscalers/include/ffx_upscale.h", to = "include/upscalers/include/ffx_upscale.h",
     sha256 = "65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf"},
    {from = "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll", to = "bin/amd_fidelityfx_loader_dx12.dll",
     sha256 = "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa"},
    {from = "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll", to = "bin/amd_fidelityfx_upscaler_dx12.dll",
     sha256 = "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46"},
    {from = "Kits/FidelityFX/docs/license.md", to = "license.md",
     sha256 = "6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963"},
}

task("setup")
    -- An explicit (even empty) options table is required for -P to work with this task: a
    -- set_menu with no "options" key rejects -P as an unrecognized option outright.
    set_menu {usage = "xmake setup", description = "fetch the pinned third-party dependencies",
              options = {}}
    on_run(function ()
        import("net.http")
        import("utils.archive")
        local thirdparty = path.join(os.projectdir(), "ThirdParty")

        -- Downloads a pinned archive and checks its SHA-256 before extracting it. Returns false when
        -- ThirdParty/<name> already holds that archive's files, else the directory it was extracted
        -- to; the caller copies what it keeps into ThirdParty/<name> and then calls finish_pin. The
        -- marker finish_pin writes is the last step, so an interrupted run is fetched again, and the
        -- marker counts only while the sentinel, a file the pin keeps (relative to its directory),
        -- still exists. xmake's own downloader, extractor and hash work the same on every host;
        -- utils.archive picks the extractor by the extension, so a NuGet package is saved under a
        -- .zip name. With unzip set, the system unzip extracts instead.
        local function fetch_pin(name, url, sha256, archive_name, sentinel, unzip)
            local dir = path.join(thirdparty, name)
            local marker = path.join(dir, ".archive-sha256")
            if os.isfile(marker) and io.readfile(marker):trim() == sha256 and os.isfile(path.join(dir, sentinel)) then
                return false
            end
            os.tryrm(dir)
            os.mkdir(thirdparty)
            local download = path.join(thirdparty, archive_name)
            os.tryrm(download)
            http.download(url, download)
            local actual = hash.sha256(download)
            if actual ~= sha256 then
                os.tryrm(download)
                raise("%s checksum mismatch; expected %s, got %s", url, sha256, actual)
            end
            local extract = path.join(thirdparty, ".extract-" .. name)
            os.tryrm(extract)
            if unzip then
                -- utils.archive is not relied on to keep executable bits, which a macOS compiler needs.
                os.mkdir(extract)
                os.execv("unzip", {"-q", "-o", download, "-d", extract})
            else
                archive.extract(download, extract)
            end
            os.rm(download)
            return extract
        end

        local function finish_pin(name, sha256, extract)
            os.rm(extract)
            io.writefile(path.join(thirdparty, name, ".archive-sha256"), sha256 .. "\n")
        end

        -- Fetches a pin made of single files instead of one archive: each file is downloaded and checked by its own
        -- SHA-256, and ThirdParty/<name> is replaced only once every file matched. The marker lists each file's hash and
        -- kept path, so a changed list is fetched again, and it counts only while every file it lists still exists.
        -- Returns the number of files fetched, 0 when ThirdParty/<name> already holds them.
        local function fetch_files(name, base_url, files)
            local dir = path.join(thirdparty, name)
            local marker = path.join(dir, ".archive-sha256")
            local lines = {}
            for _, file in ipairs(files) do
                table.insert(lines, file.sha256 .. "  " .. file.to)
            end
            local expected = table.concat(lines, "\n")
            local kept = os.isfile(marker) and (io.readfile(marker):gsub("\r", "")):trim() == expected
            for _, file in ipairs(files) do
                kept = kept and os.isfile(path.join(dir, file.to))
            end
            if kept then
                return 0
            end
            local fetch = path.join(thirdparty, ".fetch-" .. name)
            os.tryrm(fetch)
            for _, file in ipairs(files) do
                local url = base_url .. "/" .. file.from
                local download = path.join(fetch, file.to)
                os.mkdir(path.directory(download))
                http.download(url, download)
                local actual = hash.sha256(download)
                if actual ~= file.sha256 then
                    os.tryrm(fetch)
                    raise("%s checksum mismatch; expected %s, got %s", url, file.sha256, actual)
                end
            end
            os.tryrm(dir)
            os.mv(fetch, dir)
            io.writefile(marker, expected .. "\n")
            return #files
        end

        if is_host("windows") then
            -- Slang and the D3D12 runtime are fetched on Windows: the shader checks need Slang and
            -- no Metal toolchain, and the D3D12 executables load the Agility SDK and WARP.
            local agility_url = format("https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/%s/microsoft.direct3d.d3d12.%s.nupkg",
                                       agility_pin, agility_pin)
            local agility_extract = fetch_pin("agility", agility_url, agility_sha256, format("agility-%s.zip", agility_pin),
                                              path.join("bin", "D3D12Core.dll"))
            if agility_extract then
                local agility = path.join(thirdparty, "agility")
                os.mkdir(path.join(agility, "bin"))
                os.cp(path.join(agility_extract, "build", "native", "include"), path.join(agility, "include"))
                os.cp(path.join(agility_extract, "build", "native", "bin", "x64", "D3D12Core.dll"), path.join(agility, "bin"))
                os.cp(path.join(agility_extract, "build", "native", "bin", "x64", "d3d12SDKLayers.dll"), path.join(agility, "bin"))
                os.cp(path.join(agility_extract, "LICENSE.txt"), agility)
                finish_pin("agility", agility_sha256, agility_extract)
            end
            local warp_url = format("https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/%s/microsoft.direct3d.warp.%s.nupkg",
                                    warp_pin, warp_pin)
            local warp_extract = fetch_pin("warp", warp_url, warp_sha256, format("warp-%s.zip", warp_pin),
                                           path.join("bin", "d3d10warp.dll"))
            if warp_extract then
                local warp = path.join(thirdparty, "warp")
                os.mkdir(path.join(warp, "bin"))
                os.cp(path.join(warp_extract, "build", "native", "bin", "x64", "d3d10warp.dll"), path.join(warp, "bin"))
                os.cp(path.join(warp_extract, "LICENSE.TXT"), warp)
                finish_pin("warp", warp_sha256, warp_extract)
            end
            local fidelityfx_url = format("https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/%s",
                                          fidelityfx_pin)
            local fidelityfx_fetched = fetch_files("fidelityfx", fidelityfx_url, fidelityfx_files)
            if fidelityfx_fetched > 0 then
                print("fidelityfx %s: %d files fetched, each matched its SHA-256", fidelityfx_pin, fidelityfx_fetched)
            end

            local slang = path.join(thirdparty, "slang")
            local slangc = path.join(slang, "bin", "slangc.exe")
            local slang_zip = format("slang-%s-windows-x86_64.zip", slang_pin:sub(2))
            local slang_url = format("https://github.com/shader-slang/slang/releases/download/%s/%s",
                                     slang_pin, slang_zip)
            local slang_extract = fetch_pin("slang", slang_url, slang_windows_archive_sha256, slang_zip,
                                            path.join("bin", "slangc.exe"))
            if slang_extract then
                os.mkdir(slang)
                os.cp(path.join(slang_extract, "*"), slang)
                finish_pin("slang", slang_windows_archive_sha256, slang_extract)
            end
            local slang_hash = hash.sha256(slangc)
            assert(slang_hash == slang_windows_sha256,
                   format("%s checksum mismatch; expected %s, got %s", slangc, slang_windows_sha256, slang_hash))

            local dxc_url = format("https://github.com/microsoft/DirectXShaderCompiler/releases/download/%s/%s",
                                   dxc_pin, dxc_archive)
            local dxc_extract = fetch_pin("dxc", dxc_url, dxc_sha256, dxc_archive, path.join("bin", "x64", "dxc.exe"))
            -- The pin counts only while all three binaries are kept; a partial directory is fetched again.
            if not dxc_extract then
                for _, file in ipairs({"dxcompiler.dll", "dxil.dll"}) do
                    if not os.isfile(path.join(thirdparty, "dxc", "bin", "x64", file)) then
                        os.tryrm(path.join(thirdparty, "dxc"))
                        dxc_extract = fetch_pin("dxc", dxc_url, dxc_sha256, dxc_archive, path.join("bin", "x64", "dxc.exe"))
                        break
                    end
                end
            end
            if dxc_extract then
                local dxc = path.join(thirdparty, "dxc")
                os.mkdir(path.join(dxc, "bin", "x64"))
                for _, file in ipairs({"dxc.exe", "dxcompiler.dll", "dxil.dll"}) do
                    os.cp(path.join(dxc_extract, "bin", "x64", file), path.join(dxc, "bin", "x64"))
                end
                for _, license in ipairs(os.files(path.join(dxc_extract, "LICENSE*"))) do
                    os.cp(license, dxc)
                end
                finish_pin("dxc", dxc_sha256, dxc_extract)
            end
            -- Slang loads dxcompiler.dll and dxil.dll from beside slangc.exe, so a refetched Slang
            -- gets them again here.
            for _, file in ipairs({"dxc.exe", "dxcompiler.dll", "dxil.dll"}) do
                local from = path.join(thirdparty, "dxc", "bin", "x64", file)
                local to = path.join(slang, "bin", file)
                assert(os.isfile(from), format("%s is missing; delete ThirdParty/dxc and run xmake setup again", from))
                if not os.isfile(to) or hash.sha256(to) ~= hash.sha256(from) then
                    os.cp(from, to)
                end
            end
            print("setup done: slang %s, agility %s, warp %s, dxc %s, fidelityfx %s", slang_pin, agility_pin, warp_pin, dxc_pin,
                  fidelityfx_pin)
            return
        end
        if not is_host("macosx") then
            print("setup done: nothing to fetch on %s", os.host())
            return
        end
        local metalcpp = path.join(thirdparty, "metal-cpp")
        local slang = path.join(thirdparty, "slang")
        if not os.isdir(metalcpp) then
            os.mkdir(metalcpp)
            os.execv("git", {"-C", metalcpp, "init", "-q"})
            os.execv("git", {"-C", metalcpp, "remote", "add", "origin",
                             "https://github.com/apple/metal-cpp.git"})
            os.execv("git", {"-C", metalcpp, "fetch", "--depth", "1", "origin", metalcpp_commit})
            os.execv("git", {"-C", metalcpp, "checkout", "-q", "FETCH_HEAD"})
        end
        local metalcpp_head = os.iorunv("git", {"-C", metalcpp, "rev-parse", "HEAD"}):trim()
        assert(metalcpp_head == metalcpp_commit,
               format("%s is at %s; expected %s", metalcpp, metalcpp_head, metalcpp_commit))
        os.execv("git", {"-C", metalcpp, "diff", "--quiet", "HEAD", "--"})

        local slangc = path.join(slang, "bin/slangc")
        local slang_zip = format("slang-%s-macos-aarch64.zip", slang_pin:sub(2))
        local slang_url = format("https://github.com/shader-slang/slang/releases/download/%s/%s",
                                 slang_pin, slang_zip)
        local slang_extract = fetch_pin("slang", slang_url, slang_macos_archive_sha256, slang_zip,
                                        path.join("bin", "slangc"), true)
        if slang_extract then
            os.mkdir(slang)
            os.execv("cp", {"-Rp", slang_extract .. "/.", slang})
            try {
                function ()
                    os.execv("xattr", {"-dr", "com.apple.quarantine", slang})
                end
            }
            finish_pin("slang", slang_macos_archive_sha256, slang_extract)
        end
        local slang_hash = hash.sha256(slangc)
        assert(slang_hash == slang_sha256,
               format("%s checksum mismatch; expected %s, got %s", slangc, slang_sha256, slang_hash))

        print("setup done: metal-cpp %s, slang %s", metalcpp_pin, slang_pin)
    end)
