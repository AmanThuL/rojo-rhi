-- Bootstrap for a standalone configure of this component: the two vendored dependencies its
-- targets compile and run against, at the same pins a host project uses. A host has a setup task
-- of its own and never includes this file, so exactly one `setup` task exists per project root.
--
-- Everything lands in <project root>/ThirdParty, which is the directory the targets default to
-- when no host overrides it.
local metalcpp_pin = "release/metal-cpp_macOS26.4_iOS26.4"
local metalcpp_commit = "c595afef4a5dc388f4047cd0c69f9e7f9468d9ed"
local slang_pin    = "v2026.14.1"
local slang_sha256 = "a1c5ecae0d2425b13fe7f616686f2df7cc7028d3f6a85fb717497cf98bee3d0a"

task("setup")
    -- An explicit (even empty) options table is required for -P to work with this task: a
    -- set_menu with no "options" key rejects -P as an unrecognized option outright.
    set_menu {usage = "xmake setup", description = "fetch the pinned third-party dependencies",
              options = {}}
    on_run(function ()
        local thirdparty = path.join(os.projectdir(), "ThirdParty")
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
