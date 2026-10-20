-- Bootstrap for a standalone configure of this component: the vendored dependencies its targets
-- compile and run against, at the same pins a host project uses. A host has a setup task of its
-- own and never includes this file, so exactly one `setup` task exists per project root.
--
-- Everything lands in <project root>/ThirdParty, which is the directory the targets default to
-- when no host overrides it.
local metalcpp_pin = "release/metal-cpp_macOS26.4_iOS26.4"
local metalcpp_commit = "c595afef4a5dc388f4047cd0c69f9e7f9468d9ed"
local slang_pin    = "v2026.14.1"
local slang_sha256 = "a1c5ecae0d2425b13fe7f616686f2df7cc7028d3f6a85fb717497cf98bee3d0a"
-- The Windows archive's compiler, checked by the hash of bin/slangc.exe.
local slang_windows_sha256 = "b9f786a651569aa4f968e4014d04b6e2f4f1a6c9584f47ea9f4ccc841fdeeeeb"
-- The D3D12 runtime a Windows executable loads from beside itself: the Agility SDK (its headers and
-- D3D12Core.dll with the debug layer) and WARP, the software adapter hosted CI runs on. Each pin is
-- the SHA-256 of the NuGet package, checked before anything is extracted from it.
local agility_pin    = "1.619.6"
local agility_sha256 = "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9"
local warp_pin       = "1.0.21"
local warp_sha256    = "ea44b77eb30eec14427193e20f40cdb2e4c31ed11ab39cb880538fdf4bac2681"

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
        -- .zip name.
        local function fetch_pin(name, url, sha256, archive_name, sentinel)
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
            archive.extract(download, extract)
            os.rm(download)
            return extract
        end

        local function finish_pin(name, sha256, extract)
            os.rm(extract)
            io.writefile(path.join(thirdparty, name, ".archive-sha256"), sha256 .. "\n")
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

            local slang = path.join(os.projectdir(), "ThirdParty", "slang")
            local slangc = path.join(slang, "bin", "slangc.exe")
            if not os.isfile(slangc) then
                local zip = format("slang-%s-windows-x86_64.zip", slang_pin:sub(2))
                local url = format("https://github.com/shader-slang/slang/releases/download/%s/%s",
                                   slang_pin, zip)
                local download = path.join(os.projectdir(), "ThirdParty", zip)
                os.mkdir(slang)
                http.download(url, download)
                archive.extract(download, slang)
                os.rm(download)
            end
            local slang_hash = hash.sha256(slangc)
            assert(slang_hash == slang_windows_sha256,
                   format("%s checksum mismatch; expected %s, got %s", slangc, slang_windows_sha256, slang_hash))
            print("setup done: slang %s, agility %s, warp %s", slang_pin, agility_pin, warp_pin)
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
        if not os.isfile(slangc) then
            local ver = slang_pin:sub(2)  -- strip leading v
            local zip = format("slang-%s-macos-aarch64.zip", ver)
            local url = format("https://github.com/shader-slang/slang/releases/download/%s/%s",
                               slang_pin, zip)
            os.mkdir(slang)
            local archive = path.join(thirdparty, zip)
            os.execv("curl", {"-L", "--max-time", "600", "-o", archive, url})
            os.execv("unzip", {"-q", "-o", archive, "-d", slang})
            os.rm(archive)
            try {
                function ()
                    os.execv("xattr", {"-dr", "com.apple.quarantine", slang})
                end
            }
        end
        local slang_hash = os.iorunv("shasum", {"-a", "256", slangc}):match("^(%x+)")
        assert(slang_hash == slang_sha256,
               format("%s checksum mismatch; expected %s", slangc, slang_sha256))

        print("setup done: metal-cpp %s, slang %s", metalcpp_pin, slang_pin)
    end)
