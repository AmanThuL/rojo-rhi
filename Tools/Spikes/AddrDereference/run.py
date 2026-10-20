"""Build and run spike 1 on D3D12; throwaway spike source.

    python Tools/Spikes/AddrDereference/run.py build [--out DIR] [--deps DIR] [--adapter A]
    python Tools/Spikes/AddrDereference/run.py correctness [--out DIR] [--adapter A]
    python Tools/Spikes/AddrDereference/run.py timing [--out DIR] [--adapter A]
    python Tools/Spikes/AddrDereference/run.py analyze [--out DIR]
    python Tools/Spikes/AddrDereference/run.py selftest

OUT defaults to build/spike-1 and DEPS to build/spike-1-deps under the repository; the adapter
is hardware (the first high-performance one without the software flag) or warp. `build` fetches
the pinned toolchain, checking each archive's SHA-256 before extracting it, compiles the 24
kernels and the harness, counts the DXIL instructions of each kernel and records the
environment. `timing` and `analyze` write under OUT/timing and OUT; `selftest` checks the analysis,
the decision rule, the discard rules and the instruction counter on synthetic data and runs
anywhere. `build`, `correctness` and `timing` are Windows only, with MSVC on PATH for `build`. The protocol is fixed in
docs/research/2026-10-20-spike-1-d3d12-addr-dereference.md.
"""
import argparse
import contextlib
import hashlib
import io
import json
import platform
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
from unittest import mock
import zipfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]

# Name, version, URL, SHA-256 of the downloaded archive, and the archive paths kept on extraction
# (a directory keeps everything under it; an empty list keeps the whole archive).
PINS = [
    {
        "name": "agility",
        "package": "Microsoft.Direct3D.D3D12",
        "version": "1.619.6",
        "url": "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.6",
        "sha256": "08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9",
        "keep": ["build/native/include/", "build/native/bin/x64/D3D12Core.dll",
                 "build/native/bin/x64/d3d12SDKLayers.dll"],
    },
    {
        "name": "warp",
        "package": "Microsoft.Direct3D.WARP",
        "version": "1.0.21",
        "url": "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.WARP/1.0.21",
        "sha256": "ea44b77eb30eec14427193e20f40cdb2e4c31ed11ab39cb880538fdf4bac2681",
        "keep": ["build/native/bin/x64/d3d10warp.dll"],
    },
    {
        "name": "dxc",
        "package": "microsoft/DirectXShaderCompiler",
        "version": "v1.9.2607",
        "url": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
               "dxc_2026_07_29.zip",
        "sha256": "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241",
        "keep": ["bin/x64/dxc.exe", "bin/x64/dxcompiler.dll", "bin/x64/dxil.dll"],
    },
    {
        "name": "slang",
        "package": "shader-slang/slang",
        "version": "v2026.14.1",
        "url": "https://github.com/shader-slang/slang/releases/download/v2026.14.1/"
               "slang-2026.14.1-windows-x86_64.zip",
        "sha256": "5ed0a59d650a0af0aca45d5db4e083b3d8fb5cea05748747dd95dfbe9c580658",
        "keep": [],
    },
]

# Arm name and the -D ARM value of Kernels.hlsl (None: Kernels.slang); base modes; variants.
ARMS = [("F", 0), ("H1", 1), ("H2", 2), ("H1m", None)]
MODES = ["u", "w", "d"]
VARIANTS = ["fetch", "alu"]

CL_FLAGS = ["/nologo", "/std:c++20", "/EHsc", "/O2", "/DNDEBUG", "/W4", "/WX", "/permissive-",
            "/utf-8", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DUNICODE", "/external:W0",
            "/external:env:INCLUDE"]
CL_LIBS = ["d3d12.lib", "dxgi.lib", "dxguid.lib"]
DXC_FLAGS = ["-T", "cs_6_6", "-E", "spikeMain", "-HV", "2021", "-O3", "-WX"]
SLANG_FLAGS = ["-I", str(ROOT / "Shaders" / "RojoRHI"), "-D", "ROJORHI_TARGET_HLSL", "-profile",
               "sm_6_6", "-entry", "spikeMain", "-stage", "compute", "-warnings-as-errors", "all"]


def sh(argv, **kw):
    return subprocess.run([str(a) for a in argv], text=True, capture_output=True, **kw)


def run_tool(argv, label: str, **kw) -> subprocess.CompletedProcess:
    """Runs a compiler step; a non-zero exit or any text on standard error stops the build."""
    result = sh(argv, **kw)
    if result.returncode or result.stderr.strip():
        print(f"{label} failed (exit {result.returncode}):\n  {' '.join(map(str, argv))}\n"
              f"{result.stdout}{result.stderr}")
        sys.exit(1)
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(pin: dict, deps: Path) -> Path:
    """Downloads the pin's archive unless cached, checks its hash, then extracts the kept paths
    into a fresh <deps>/<name>/. A wrong hash stops the build before anything is extracted."""
    archives = deps / "archives"
    archives.mkdir(parents=True, exist_ok=True)
    archive = archives / f"{pin['name']}-{pin['version']}.zip"
    if not archive.exists():
        partial = archive.with_suffix(".part")
        with urllib.request.urlopen(pin["url"], timeout=600) as response, partial.open("wb") as file:
            shutil.copyfileobj(response, file)
        partial.replace(archive)
    actual = sha256(archive)
    if actual != pin["sha256"]:
        archive.unlink()
        sys.exit(f"pin {pin['name']} {pin['version']} sha256 {actual} MISMATCH, expected "
                 f"{pin['sha256']}; the archive was deleted and nothing was extracted")
    print(f"pin {pin['name']} {pin['version']} sha256 {actual} ok")
    target = deps / pin["name"]
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    kept = set()
    with zipfile.ZipFile(archive) as zip_file:
        for info in zip_file.infolist():
            # The DXC archive names its members with backslashes.
            name = info.filename.replace("\\", "/")
            if info.is_dir() or name.endswith("/"):
                continue
            keep = not pin["keep"] or any(name == k or (k.endswith("/") and name.startswith(k))
                                          for k in pin["keep"])
            if not keep:
                continue
            parts = Path(name).parts
            if Path(name).is_absolute() or ".." in parts or ":" in name:
                sys.exit(f"pin {pin['name']}: unsafe archive member {info.filename!r}")
            destination = target.joinpath(*parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with zip_file.open(info) as source, destination.open("wb") as file:
                shutil.copyfileobj(source, file)
            kept.add(name)
    for k in pin["keep"]:
        if not any(name == k or (k.endswith("/") and name.startswith(k)) for name in kept):
            sys.exit(f"pin {pin['name']}: the archive holds no {k}")
    return target


def setup_slang_windows_sha256() -> str:
    """The hash of bin/slangc.exe that xmake/setup.lua pins for the Windows archive."""
    text = (ROOT / "xmake" / "setup.lua").read_text(encoding="utf-8")
    match = re.search(r'\bslang_windows_sha256\s*=\s*"([0-9a-f]{64})"', text)
    if not match:
        sys.exit("xmake/setup.lua pins no slang_windows_sha256")
    return match.group(1)


def toolchain(deps: Path) -> dict:
    paths = {pin["name"]: fetch(pin, deps) for pin in PINS}
    slangc = paths["slang"] / "bin" / "slangc.exe"
    expected = setup_slang_windows_sha256()
    actual = sha256(slangc)
    if actual != expected:
        sys.exit(f"bin/slangc.exe sha256 {actual} differs from setup.lua's {expected}")
    print(f"slangc.exe sha256 {actual} ok (xmake/setup.lua)")
    dxc_bin = paths["dxc"] / "bin" / "x64"
    # Slang emits DXIL through the DXC it finds beside slangc.exe, so that is the pinned one.
    for name in ("dxcompiler.dll", "dxil.dll"):
        shutil.copy2(dxc_bin / name, slangc.parent / name)
    return {
        "include": paths["agility"] / "build" / "native" / "include",
        "agilityBin": paths["agility"] / "build" / "native" / "bin" / "x64",
        "warp": paths["warp"] / "build" / "native" / "bin" / "x64" / "d3d10warp.dll",
        "dxc": dxc_bin / "dxc.exe",
        "slangc": slangc,
    }


ENTRY = re.compile(r"^define\b.*@spikeMain\(")
LABEL = re.compile(r"^[A-Za-z0-9_.$\"-]+:(\s*;.*)?$")
DX_OP = re.compile(r"@(dx\.op\.[A-Za-z0-9]+)")


def count_dxil(text: str) -> dict:
    """Instruction count of `dxc -dumpbin` text: the lines of the entry function's body that are
    not blank, a label or a comment, and a histogram of the @dx.op.* names called there (the
    overload suffix dropped, so @dx.op.rawBufferLoad.i32 counts as dx.op.rawBufferLoad)."""
    instructions = 0
    ops: dict = {}
    inside = False
    for raw in text.splitlines():
        line = raw.strip()
        if not inside:
            inside = bool(ENTRY.match(line))
            continue
        if line == "}":
            break
        if not line or line.startswith(";") or LABEL.match(line):
            continue
        instructions += 1
        for op in DX_OP.findall(line):
            ops[op] = ops.get(op, 0) + 1
    if not inside:
        raise ValueError("no `define ... @spikeMain(` in the disassembly")
    return {"instructions": instructions, "ops": dict(sorted(ops.items()))}


def kernels(tools: dict, out: Path) -> dict:
    folder = out / "kernels"
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    counts = {}
    for arm, arm_value in ARMS:
        for mode, mode_name in enumerate(MODES):
            for alu, variant in enumerate(VARIANTS):
                name = f"{arm}-{mode_name}-{variant}"
                blob = folder / f"{name}.dxil"
                defines = ["-D", f"MODE={mode}", "-D", f"ALU={alu}"]
                if arm_value is None:
                    base = [tools["slangc"], SOURCE / "Kernels.slang", *SLANG_FLAGS, *defines]
                    run_tool([*base, "-target", "dxil", "-o", blob], f"slangc {name}")
                    run_tool([*base, "-target", "hlsl", "-o", folder / f"{name}.hlsl"],
                             f"slangc -target hlsl {name}")
                else:
                    run_tool([tools["dxc"], *DXC_FLAGS, "-D", f"ARM={arm_value}", *defines,
                              SOURCE / "Kernels.hlsl", "-Fo", blob], f"dxc {name}")
                text = run_tool([tools["dxc"], "-dumpbin", blob], f"dxc -dumpbin {name}").stdout
                (folder / f"{name}.txt").write_text(text, encoding="utf-8")
                counts[name] = count_dxil(text)
                print(f"kernel {name}: {counts[name]['instructions']} instructions")
    (folder / "counts.json").write_text(json.dumps(counts, indent=2) + "\n")
    return counts


def harness(tools: dict, out: Path) -> None:
    bin_dir = out / "bin"
    objects = out / "obj"
    for folder in (bin_dir / "D3D12", objects):
        folder.mkdir(parents=True, exist_ok=True)
    result = sh(["cl", *CL_FLAGS, "/external:I", tools["include"], SOURCE / "Harness.cpp",
                 f"/Fo{objects / 'Harness.obj'}", f"/Fe{bin_dir / 'Harness.exe'}", "/link", *CL_LIBS],
                cwd=objects)
    print(result.stdout + result.stderr, end="")
    if result.returncode:
        sys.exit(f"cl failed (exit {result.returncode})")
    print("(the linker's import library note comes from the Agility SDK exports, D3D12SDKVersion "
          "and D3D12SDKPath, which the loader reads; it is not a warning)")
    # The Agility runtime goes where D3D12SDKPath points, WARP beside the executable.
    for name in ("D3D12Core.dll", "d3d12SDKLayers.dll"):
        shutil.copy2(tools["agilityBin"] / name, bin_dir / "D3D12" / name)
    shutil.copy2(tools["warp"], bin_dir / "d3d10warp.dll")


def msvc_version() -> str:
    """The compiler version from cl's banner, "Version 19.51.36260" for example."""
    banner = sh(["cl"]).stderr
    match = re.search(r"Version (\S+)", banner)
    if not match:
        sys.exit(f"cl printed no version in its banner:\n{banner}")
    return match.group(1)


def limits(out: Path, adapter: str) -> dict:
    result = sh([out / "bin" / "Harness.exe", "limits", "--adapter", adapter], cwd=out / "bin")
    if result.returncode or result.stderr.strip():
        sys.exit(f"Harness limits --adapter {adapter} failed (exit {result.returncode}):\n"
                 f"{result.stdout}{result.stderr}")
    data = json.loads(result.stdout)
    (out / "limits.json").write_text(json.dumps(data, indent=2) + "\n")
    return data


def build(args) -> None:
    out, deps = args.out.resolve(), args.deps.resolve()
    out.mkdir(parents=True, exist_ok=True)
    tools = toolchain(deps)
    kernels(tools, out)
    harness(tools, out)
    dxc_version = sh([tools["dxc"], "--version"])
    slang_version = sh([tools["slangc"], "-version"])
    environment = {
        "windowsBuild": platform.version(),
        "msvc": msvc_version(),
        "dxc": (dxc_version.stdout + dxc_version.stderr).strip(),
        "slang": (slang_version.stdout + slang_version.stderr).strip(),
        "pins": [{key: pin[key] for key in ("name", "package", "version", "url", "sha256")}
                 for pin in PINS],
        "spikeCommit": sh(["git", "-C", ROOT, "rev-parse", "HEAD"]).stdout.strip(),
        "limits": limits(out, args.adapter),
    }
    (out / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    print(json.dumps(environment, indent=2))


SWEEP = [64, 1024, 16384, 262144, 1048576]
KERNEL_COUNT = len(ARMS) * len(MODES)
SEVERE = ("corruption", "error", "warning")
PROCESS_TIMEOUT = 3600


def line_buffered() -> None:
    """Line-buffers standard output when it is a real stream, so CI logs follow progress."""
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)


def corrupted_threads(threads: int, mode: str) -> int:
    """How many threads read a word that the harness's --corrupt-data changes (word 0 of
    elements 0 to 63 and of element 64k of every slice k), for the slice `mode` gives thread t."""
    count = 0
    for t in range(threads):
        k = 0 if mode == "u" else (t >> 6) & 63 if mode == "w" else t & 63
        elements = [(j << 12) | (t & 4095) for j in range(16)]
        count += any(e < 64 or e == 64 * k for e in elements)
    return count


def harness_process(out: Path, argv: list, log: Path) -> dict:
    """Runs one Harness process from the binary directory and writes its log; returns the exit
    code, the stderr lines, the seconds it took and its stdout."""
    start = time.monotonic()
    result = sh([out / "bin" / "Harness.exe", *argv], cwd=out / "bin", timeout=PROCESS_TIMEOUT)
    seconds = time.monotonic() - start
    log.write_text(f"$ Harness {' '.join(map(str, argv))}\nexit {result.returncode}\n"
                   f"--- stdout\n{result.stdout}--- stderr\n{result.stderr}", encoding="utf-8")
    return {"exit": result.returncode, "seconds": seconds, "stdout": result.stdout,
            "stderrLines": [line for line in result.stderr.splitlines() if line.strip()]}


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def run_configuration(out: Path, adapter: str, threads: int, variant: str, name: str,
                      extra: list) -> tuple:
    folder = out / "correctness"
    result_path = folder / f"{name}.json"
    process = harness_process(out, ["run", "--adapter", adapter, "--kernels", out / "kernels",
                                    "--threads", threads, "--variant", variant, "--order", 0,
                                    "--mode", "correctness", "--out", result_path, *extra],
                              folder / f"{name}.log")
    return process, read_json(result_path)


def correctness(args) -> None:
    """The correctness gate: every configuration once, order 0, under the debug layer and
    GPU-based validation; then the negative check; then the view-cap side probe."""
    line_buffered()
    out = args.out.resolve()
    if not (out / "bin" / "Harness.exe").exists() or not (out / "kernels").is_dir():
        sys.exit(f"run.py correctness: no harness or kernels under {out}; run build first")
    folder = out / "correctness"
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)

    passed = True
    configurations = []
    for threads in SWEEP:
        for variant in VARIANTS:
            name = f"n{threads}-{variant}"
            process, data = run_configuration(out, args.adapter, threads, variant, name, [])
            kernels_run = data["kernels"] if data else []
            mismatches = sum(k["mismatches"] for k in kernels_run)
            messages = data["messages"] if data else {}
            severe = sum(messages.get(s, 0) for s in SEVERE)
            problems = []
            if process["exit"]:
                problems.append(f"exit {process['exit']}")
            if data is None:
                problems.append("no result JSON")
            elif len(kernels_run) != KERNEL_COUNT:
                problems.append(f"{len(kernels_run)} of {KERNEL_COUNT} kernels ran")
            if mismatches:
                bad = [f"{k['name']} (first thread {k['firstBad']})" for k in kernels_run
                       if k["mismatches"]]
                problems.append("mismatches in " + ", ".join(bad))
            if severe:
                problems.append(f"{severe} messages of severity warning or worse")
            if process["stderrLines"]:
                problems.append(f"{len(process['stderrLines'])} stderr lines")
            ok = not problems
            passed = passed and ok
            print(f"{name}: {len(kernels_run)} cases, {mismatches} mismatches, {severe} messages "
                  f"(info {messages.get('info', 0)}), {len(process['stderrLines'])} stderr lines, "
                  f"{process['seconds']:.1f} s: {'ok' if ok else 'FAILED: ' + '; '.join(problems)}")
            for line in process["stderrLines"][:20]:
                print(f"  stderr: {line}")
            configurations.append({"name": name, "threads": threads, "variant": variant,
                                   "passed": ok, "cases": len(kernels_run),
                                   "mismatches": mismatches, "messages": messages,
                                   "stderrLines": len(process["stderrLines"]),
                                   "seconds": round(process["seconds"], 3), "problems": problems})

    # The gate must be able to fail: with words of every slice changed on the GPU, every kernel
    # has to mismatch in exactly the threads that read a changed word, and the process has to
    # exit non-zero.
    negative_threads = 1024
    process, data = run_configuration(out, args.adapter, negative_threads, "fetch", "negative",
                                      ["--corrupt-data"])
    kernels_run = data["kernels"] if data else []
    expected = {mode: corrupted_threads(negative_threads, mode) for mode in MODES}
    missed = [f"{k['name']} ({k['mismatches']} mismatches, expected "
              f"{expected[k['name'].rsplit('-', 1)[1]]})" for k in kernels_run
              if k["mismatches"] != expected[k["name"].rsplit("-", 1)[1]]]
    negative_ok = (process["exit"] != 0 and data is not None
                   and len(kernels_run) == KERNEL_COUNT and not missed)
    if negative_ok:
        print("negative check: every kernel failed as expected ("
              + ", ".join(f"{k['name']} {k['mismatches']}" for k in kernels_run) + ")")
    else:
        reasons = []
        if process["exit"] == 0:
            reasons.append("the process exited 0")
        if data is None:
            reasons.append("no result JSON")
        elif len(kernels_run) != KERNEL_COUNT:
            reasons.append(f"{len(kernels_run)} of {KERNEL_COUNT} kernels ran")
        if missed:
            reasons.append("wrong mismatch count in " + ", ".join(missed))
        print("negative check: FAILED: " + "; ".join(reasons))
    passed = passed and negative_ok

    print(f"correctness gate: {'passed' if passed else 'FAILED'}")
    summary = {"passed": passed, "adapter": args.adapter, "configurations": configurations,
               "negative": {"passed": negative_ok, "exit": process["exit"],
                            "expectedMismatches": expected, "kernelsOff": missed}}
    (folder / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    # The view-cap side probe reports what the debug layer says and never changes the gate.
    viewcap_path = folder / "viewcap.json"
    process = harness_process(out, ["viewcap", "--adapter", args.adapter, "--out", viewcap_path],
                              folder / "viewcap.log")
    probe = read_json(viewcap_path)
    if process["exit"] or probe is None:
        outcome = f"did not run (exit {process['exit']}): {' '.join(process['stderrLines'])}"
    elif probe["raised"]:
        outcome = "; ".join(f"{m['severity']} #{m['id']}: {m['text']}" for m in probe["messages"]
                            if m["severity"] in SEVERE)
    else:
        outcome = "no message of severity warning or worse"
    if probe is not None and probe.get("deviceRemoved"):
        outcome += f"; device removed ({probe['deviceRemovedReason']})"
    print(f"viewcap (report-only), a raw SRV of 2^27 + 1 elements: {outcome}")
    summary["viewcap"] = {"ran": probe is not None and not process["exit"], "outcome": outcome}
    (folder / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    sys.exit(0 if passed else 1)


# ---- timing ------------------------------------------------------------------------------------

PROCESSES = 20
ATTEMPTS = 4
DISCARD_REST_SECONDS = 120
NO_CLI_REST_SECONDS = 30
MAX_TEMPERATURE_RISE = 10
MIN_CLOCK_SHARE = 0.95
HIGH_PERFORMANCE = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"
ULTIMATE_PERFORMANCE = "e9a42b02-d5df-448d-aa00-03f14749eb61"
HAGS_KEY = r"HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers"
SEED = 20261020
RESAMPLES = 10000
BUDGET = 0.05
JUDGED = (1048576, "fetch")
MODE_NAMES = {"u": "uniform", "w": "wave-uniform", "d": "divergent"}


def on_ac_power() -> bool:
    """True when GetSystemPowerStatus reports AC line power; Windows only."""
    import ctypes

    class PowerStatus(ctypes.Structure):
        _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte),
                    ("BatteryLifePercent", ctypes.c_ubyte), ("SystemStatusFlag", ctypes.c_ubyte),
                    ("BatteryLifeTime", ctypes.c_ulong), ("BatteryFullLifeTime", ctypes.c_ulong)]

    status = PowerStatus()
    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(status)):
        sys.exit("GetSystemPowerStatus failed")
    return status.ACLineStatus == 1


def power_scheme() -> dict:
    text = sh(["powercfg", "/getactivescheme"]).stdout
    match = re.search(r"([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})\s*(?:\((.*?)\))?", text)
    if not match:
        sys.exit(f"powercfg /getactivescheme printed no scheme GUID:\n{text}")
    return {"guid": match.group(1).lower(), "name": match.group(2) or ""}


def hags_setting() -> str:
    """HwSchMode (2 on, 1 off) as the registry holds it, or "unknown" when the value is absent."""
    result = sh(["reg", "query", HAGS_KEY, "/v", "HwSchMode"])
    match = re.search(r"HwSchMode\s+REG_DWORD\s+(0x[0-9a-fA-F]+)", result.stdout)
    return match.group(1) if match and result.returncode == 0 else "unknown"


def gpu_reading():
    """(temperature in C, SM clock in MHz) from nvidia-smi, or None where it gives none."""
    result = sh(["nvidia-smi", "--query-gpu=temperature.gpu,clocks.sm",
                 "--format=csv,noheader,nounits"])
    match = re.match(r"\s*(\d+)\s*,\s*(\d+)", result.stdout)
    if result.returncode or not match:
        return None
    return int(match.group(1)), int(match.group(2))


def discard_reason(run: dict, start, end, start_clocks: list):
    """Why a timed process is discarded, or None. `start` and `end` are (temperature, clock)
    readings or None without a vendor tool; `start_clocks` holds the start clocks of the round's
    kept processes so far plus this one; discarded attempts are not counted."""
    if not run["powerOnAcStart"] or not run["powerOnAcEnd"]:
        return "off AC power at its start or end"
    if start is None or end is None:
        return None
    if end[0] - start[0] > MAX_TEMPERATURE_RISE:
        return f"temperature rose from {start[0]} to {end[0]} C"
    reference = statistics.median(start_clocks)
    if end[1] < MIN_CLOCK_SHARE * reference:
        return f"end clock {end[1]} MHz is below 95% of the median start clock {reference} MHz"
    return None


def tail(path: Path, lines: int = 25) -> str:
    return "\n".join(path.read_text(encoding="utf-8", errors="replace").splitlines()[-lines:])


def timing(args) -> None:
    """Refuses unless the setup the note requires holds, then runs 20 processes of each of the
    10 configurations, in process order, resumably."""
    line_buffered()
    if args.adapter == "warp":
        sys.exit("run.py timing: refused: WARP measures nothing and is never timed")
    out = args.out.resolve()
    summary = read_json(out / "correctness" / "summary.json")
    if not summary or summary.get("passed") is not True or summary.get("adapter") != args.adapter:
        sys.exit(f"run.py timing: refused: {out / 'correctness' / 'summary.json'} is missing or "
                 f"does not record a passed correctness run on the {args.adapter} adapter")
    if not (out / "bin" / "Harness.exe").exists():
        sys.exit(f"run.py timing: no harness under {out}; run build first")
    if not on_ac_power():
        sys.exit("run.py timing: refused: the machine is off AC power")
    scheme = power_scheme()
    if scheme["guid"] not in (HIGH_PERFORMANCE, ULTIMATE_PERFORMANCE):
        sys.exit(f"run.py timing: refused: the active power scheme is {scheme['guid']} "
                 f"({scheme['name']}), not High performance or Ultimate Performance")
    vendor_tool = shutil.which("nvidia-smi") is not None
    environment = read_json(out / "environment.json") or {}
    environment.update({
        "powerScheme": scheme, "hags": hags_setting(), "timingAdapter": args.adapter,
        "thermalControl": "nvidia-smi" if vendor_tool else "none: no vendor CLI"})
    (out / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")

    folder = out / "timing"
    folder.mkdir(exist_ok=True)
    # The start clocks of the kept processes, restored on resume; discarded attempts and results
    # without thermal readings do not count.
    start_clocks: list = []
    for path in folder.glob("n*-p??.json"):
        reading = ((read_json(path) or {}).get("thermal") or {}).get("start")
        if reading:
            start_clocks.append(reading[1])
    for process in range(PROCESSES):
        for threads in SWEEP:
            for variant in VARIANTS:
                name = f"n{threads}-{variant}-p{process:02d}"
                result_path = folder / f"{name}.json"
                if (folder / f"{name}.notrun.json").exists():
                    continue
                if result_path.exists():
                    if "thermal" in (read_json(result_path) or {}):
                        continue
                    # Left by a process that stopped the round: not kept, so it runs again.
                    result_path.replace(folder / f"{name}.failed.json")
                # A resumed process continues its attempts; earlier discards count and stay.
                first_attempt = len(list(folder.glob(f"{name}.discarded*.json"))) + 1
                for attempt in range(first_attempt, ATTEMPTS + 1):
                    start = gpu_reading() if vendor_tool else None
                    run = harness_process(
                        out, ["run", "--adapter", args.adapter, "--kernels", out / "kernels",
                              "--threads", threads, "--variant", variant, "--order", process,
                              "--mode", "timing", "--out", result_path], folder / f"{name}.log")
                    end = gpu_reading() if vendor_tool else None
                    data = read_json(result_path)
                    if run["exit"] or run["stderrLines"] or data is None \
                            or len(data["kernels"]) != KERNEL_COUNT:
                        if result_path.exists():
                            result_path.replace(folder / f"{name}.failed.json")
                        print(f"{name}: the round stops (exit {run['exit']}, "
                              f"{len(run['stderrLines'])} stderr lines); log tail:\n"
                              f"{tail(folder / (name + '.log'))}")
                        sys.exit(1)
                    data["thermal"] = {"start": start, "end": end}
                    clocks = start_clocks + ([start[1]] if start else [])
                    reason = discard_reason(data, start, end, clocks)
                    if reason is None:
                        result_path.write_text(json.dumps(data) + "\n")
                        start_clocks[:] = clocks
                        print(f"{name}: kept, attempt {attempt}")
                        break
                    print(f"{name}: discarded, attempt {attempt}: {reason}")
                    result_path.unlink()
                    (folder / f"{name}.discarded{attempt}.json").write_text(json.dumps(data) + "\n")
                    if attempt < ATTEMPTS:
                        time.sleep(DISCARD_REST_SECONDS)
                else:
                    (folder / f"{name}.notrun.json").write_text(json.dumps(
                        {"notRun": True, "attempts": ATTEMPTS}) + "\n")
                    print(f"{name}: not run after {ATTEMPTS} attempts")
                if not vendor_tool:
                    time.sleep(NO_CLI_REST_SECONDS)
    print("timing: every process done")


# ---- analysis ----------------------------------------------------------------------------------


def bootstrap(deltas: list, rng: random.Random) -> tuple:
    """Two-sided 95% percentile bootstrap interval of the median, over resampled processes."""
    stats = sorted(statistics.median(rng.choices(deltas, k=len(deltas)))
                   for _ in range(RESAMPLES))
    return stats[int(0.025 * RESAMPLES)], stats[int(0.975 * RESAMPLES) - 1]


def process_median(kernel: dict) -> float:
    return statistics.median(kernel["gpuNs"]) if kernel.get("gpuNs") else kernel["gpuMedianNs"]


def compare(rows: list, numerator: str, denominator: str, floor_name: str,
            rng: random.Random):
    """The paired delta numerator - denominator over processes, its interval, and whether it is
    a slowdown (interval above zero and delta above 5% of F's median of process medians) or a
    speedup (the mirror image). None when a kernel is missing from any process."""
    if any(numerator not in row or denominator not in row for row in rows):
        return None
    deltas = [row[numerator] - row[denominator] for row in rows]
    base = statistics.median(row[floor_name] for row in rows)
    delta = statistics.median(deltas)
    low, high = bootstrap(deltas, rng)
    return {"delta": delta, "low": low, "high": high, "relative": delta / base,
            "slowdown": low > 0 and delta > BUDGET * base,
            "speedup": high < 0 and -delta > BUDGET * base}


def outlier_shares(samples_by_process: list) -> dict:
    """Shares of samples below 0.8 and above 1.2 times their process median."""
    total = low = high = 0
    for samples in samples_by_process:
        centre = statistics.median(samples)
        total += len(samples)
        low += sum(1 for v in samples if v < 0.8 * centre)
        high += sum(1 for v in samples if v > 1.2 * centre)
    return {"below": low / total, "above": high / total}


def analyze_runs(runs: list, not_run: set, counts: dict) -> dict:
    """Everything `analyze` reports, from the timed processes' JSON (each with its `threads`,
    `variant` and `process`), the (threads, variant) pairs marked not run and counts.json. The
    bootstrap draws come from one generator in a fixed order."""
    rng = random.Random(SEED)
    groups: dict = {}
    for run in runs:
        groups.setdefault((run["threads"], run["variant"]), {})[run["process"]] = run
    configurations = {}
    not_run_list = []
    for threads in SWEEP:
        for variant in VARIANTS:
            key = (threads, variant)
            name = f"n{threads}-{variant}"
            processes = groups.get(key, {})
            if key in not_run or len(processes) != PROCESSES:
                not_run_list.append(name)
                continue
            ordered = [processes[p] for p in sorted(processes)]
            rows, samples = [], {}
            for run in ordered:
                row = {}
                for kernel in run["kernels"]:
                    if kernel.get("notRun"):
                        continue
                    row[kernel["name"]] = process_median(kernel)
                    samples.setdefault(kernel["name"], []).append(kernel["gpuNs"])
                rows.append(row)
            modes = {}
            for mode, mode_name in MODE_NAMES.items():
                f_name = f"F-{mode}"
                entry = {"medians": {}, "comparisons": {}, "ratios": {}, "outliers": {}}
                for arm, _ in ARMS:
                    kernel = f"{arm}-{mode}"
                    if all(kernel in row for row in rows):
                        entry["medians"][arm] = statistics.median(row[kernel] for row in rows)
                        if kernel in samples:
                            entry["outliers"][arm] = outlier_shares(samples[kernel])
                for label, a, b in (("H1-F", "H1", "F"), ("H2-F", "H2", "F"),
                                    ("H1m-F", "H1m", "F"), ("H1m-H1", "H1m", "H1")):
                    if all(f"F-{mode}" in row for row in rows):
                        entry["comparisons"][label] = compare(rows, f"{a}-{mode}", f"{b}-{mode}",
                                                              f_name, rng)
                for arm in ("H1", "H2", "H1m"):
                    if arm in entry["medians"] and "F" in entry["medians"]:
                        entry["ratios"][f"{arm}/F"] = entry["medians"][arm] / entry["medians"]["F"]
                modes[mode_name] = entry
            dxil = {k.rsplit("-", 2)[0] + "-" + k.rsplit("-", 2)[1]: v["instructions"]
                    for k, v in counts.items() if k.endswith("-" + variant)}
            configurations[name] = {"threads": threads, "variant": variant,
                                    "processes": len(ordered), "modes": modes,
                                    "dxilInstructions": dxil}
    return {"seed": SEED, "resamples": RESAMPLES, "budget": BUDGET,
            "configurations": configurations, "notRun": not_run_list,
            "decision": decide(configurations, not_run_list)}


def decide(configurations: dict, not_run_list: list) -> dict:
    """The decision rule at the judged configuration, each H arm against F in the same base
    mode: cheap when H1 uniform and H1 wave-uniform show no slowdown, costly otherwise,
    incomplete when a judged result is missing; escalate when H2 exceeds twice F in any base mode
    or H1 divergent exceeds three times F divergent."""
    name = f"n{JUDGED[0]}-{JUDGED[1]}"
    config = configurations.get(name)
    decision = {"configuration": name, "verdict": "incomplete", "escalate": False,
                "escalations": [], "missing": [], "slowdowns": [], "speedups": []}
    if config is None:
        decision["missing"].append(f"{name} was not run")
        return decision
    for mode in ("uniform", "wave-uniform"):
        entry = config["modes"][mode]
        for arm in ("F", "H1"):
            if arm not in entry["medians"]:
                decision["missing"].append(f"{arm} {mode}")
    for mode_name, entry in config["modes"].items():
        ratio = entry["ratios"].get("H2/F")
        if ratio is not None and ratio > 2:
            decision["escalations"].append(f"H2 {mode_name} is {ratio:.2f} times F")
    ratio = config["modes"]["divergent"]["ratios"].get("H1/F")
    if ratio is not None and ratio > 3:
        decision["escalations"].append(f"H1 divergent is {ratio:.2f} times F divergent")
    decision["escalate"] = bool(decision["escalations"])
    if decision["missing"]:
        return decision
    for mode in ("uniform", "wave-uniform"):
        result = config["modes"][mode]["comparisons"].get("H1-F")
        if result is None:
            decision["missing"].append(f"H1 - F {mode}")
        elif result["slowdown"]:
            decision["slowdowns"].append(mode)
        elif result["speedup"]:
            decision["speedups"].append(mode)
    if not decision["missing"]:
        decision["verdict"] = "costly" if decision["slowdowns"] else "cheap"
    return decision


def render(summary: dict) -> str:
    lines = [f"seed {summary['seed']}, {summary['resamples']} resamples, budget "
             f"{summary['budget']:.0%}; deltas in ns, relative to F's median of process medians"]
    for name, config in summary["configurations"].items():
        lines.append(f"\n{name} ({config['processes']} processes)")
        for mode_name, entry in config["modes"].items():
            medians = ", ".join(f"{arm} {value:.0f}" for arm, value in entry["medians"].items())
            lines.append(f"  {mode_name}: medians {medians}")
            for label, result in entry["comparisons"].items():
                if result is None:
                    lines.append(f"    {label}: no result")
                    continue
                flag = "SLOWDOWN" if result["slowdown"] else "speedup" if result["speedup"] else ""
                lines.append(f"    {label}: {result['delta']:+.1f} [{result['low']:+.1f}, "
                             f"{result['high']:+.1f}] ({result['relative']:+.1%}) {flag}".rstrip())
            ratios = ", ".join(f"{k} {v:.2f}" for k, v in entry["ratios"].items())
            lines.append(f"    ratios: {ratios}")
            shares = ", ".join(f"{arm} {v['below']:.1%}/{v['above']:.1%}"
                               for arm, v in entry["outliers"].items())
            lines.append(f"    outliers below 0.8 / above 1.2: {shares}")
        counts = ", ".join(f"{k} {v}" for k, v in sorted(config["dxilInstructions"].items()))
        lines.append(f"  DXIL instructions: {counts}")
    lines.append(f"\nnot run: {', '.join(summary['notRun']) or 'none'}")
    d = summary["decision"]
    lines.append(f"\ndecision at {d['configuration']}: {d['verdict']}"
                 + (f" (slowdown: {', '.join(d['slowdowns'])})" if d["slowdowns"] else "")
                 + (f" (speedup: {', '.join(d['speedups'])})" if d["speedups"] else "")
                 + (f" (missing: {', '.join(d['missing'])})" if d["missing"] else ""))
    lines.append("escalate: " + ("yes: " + "; ".join(d["escalations"]) if d["escalate"] else "no"))
    return "\n".join(lines) + "\n"


def collect(folder: Path) -> tuple:
    """The kept timed processes of OUT/timing and the (threads, variant) pairs marked not run."""
    runs, not_run = [], set()
    for path in sorted(folder.glob("n*-p??.json")):
        match = re.fullmatch(r"n(\d+)-(\w+)-p(\d\d)\.json", path.name)
        data = read_json(path)
        # A result without thermal readings was never judged, so it is not a kept process.
        if match and data and "thermal" in data:
            data["process"] = int(match.group(3))
            runs.append(data)
    for path in folder.glob("n*.notrun.json"):
        match = re.fullmatch(r"n(\d+)-(\w+)-p\d\d\.notrun\.json", path.name)
        if match:
            not_run.add((int(match.group(1)), match.group(2)))
    return runs, not_run


def analyze(args) -> None:
    out = args.out.resolve()
    runs, not_run = collect(out / "timing")
    if not runs:
        sys.exit(f"run.py analyze: no timed processes under {out / 'timing'}")
    counts = read_json(out / "kernels" / "counts.json") or {}
    summary = analyze_runs(runs, not_run, counts)
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    text = render(summary)
    (out / "analysis.txt").write_text(text, encoding="utf-8")
    sums = out / "SHA256SUMS"
    sums.unlink(missing_ok=True)
    lines = [f"{sha256(path)}  {path.relative_to(out).as_posix()}"
             for path in sorted(out.rglob("*")) if path.is_file()]
    sums.write_text("\n".join(lines) + "\n")
    print(text)


# ---- selftest ----------------------------------------------------------------------------------

CANNED_DUMPBIN = """; some header
define void @spikeMain() {
entry:
  %1 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)
  ; a comment line

  %2 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, i32 0)
  %3 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %2, i32 %1)
  br label %loop
loop:                                             ; preds = %entry
  %4 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %2, i32 %4)
  ret void
}
define void @other() {
  ret void
}
"""


def synthetic_runs(rng: random.Random, factors: dict, threads: int = JUDGED[0],
                   variant: str = JUDGED[1], drop: tuple = (), noise: dict = None) -> list:
    """Timed processes of one configuration: F is 1000, 1100 and 1200 ns in uniform, wave-uniform
    and divergent; an H arm is its factor times F of the same mode; every process and sample
    carries about 1% noise. `drop` names kernels left out; `noise` gives a kernel an extra
    per-process relative noise of that standard deviation."""
    noise = noise or {}
    base = {"u": 1000.0, "w": 1100.0, "d": 1200.0}
    defaults = {"F": 1.0, "H1": 1.0, "H2": 1.1, "H1m": 1.5}
    runs = []
    for process in range(PROCESSES):
        scale = 1 + rng.gauss(0, 0.01)
        kernels = []
        for arm, _ in ARMS:
            for mode in MODE_NAMES:
                name = f"{arm}-{mode}"
                if name in drop:
                    continue
                factor = factors.get(name, factors.get(arm, defaults[arm]))
                if name in noise:
                    factor *= 1 + rng.gauss(0, noise[name])
                samples = [base[mode] * factor * scale * (1 + rng.gauss(0, 0.01))
                           for _ in range(200)]
                kernels.append({"name": name, "arm": arm, "baseMode": MODE_NAMES[mode],
                                "mismatches": 0, "notRun": False,
                                "gpuMedianNs": statistics.median(samples), "gpuNs": samples})
        runs.append({"threads": threads, "variant": variant, "process": process,
                     "kernels": kernels})
    return runs


def expect(condition: bool, what: str) -> None:
    if not condition:
        sys.exit(f"selftest: FAILED: {what}")


def selftest_timing() -> None:
    """The timing loop with the harness, the GPU readings and the rests stubbed: two processes of
    one configuration, the second discarded 4 times for a low end clock and so not run; a resume
    that runs nothing; a stale result without thermal readings that runs again; and a non-zero
    exit that stops the round and leaves its result as .failed.json."""
    state = {"orders": [], "exit": 0, "readings": 0, "low": {1}}

    def fake_harness(out, argv, log):
        order = argv[argv.index("--order") + 1]
        state["orders"].append(order)
        kernels = [{"name": f"{arm}-{mode}", "mismatches": 0, "gpuNs": [1.0], "gpuMedianNs": 1.0}
                   for arm, _ in ARMS for mode in MODES]
        Path(argv[argv.index("--out") + 1]).write_text(json.dumps(
            {"kernels": kernels, "powerOnAcStart": True, "powerOnAcEnd": True}))
        log.write_text("log\n")
        return {"exit": state["exit"], "seconds": 0.0, "stdout": "", "stderrLines": []}

    def fake_reading():
        # Readings alternate start, end; a process in state["low"] ends every attempt at half the
        # start clock.
        state["readings"] += 1
        if state["readings"] % 2 == 0 and state["orders"][-1] in state["low"]:
            return 50, 1000
        return 50, 2000

    stubs = {"harness_process": fake_harness, "gpu_reading": fake_reading,
             "on_ac_power": lambda: True, "hags_setting": lambda: "unknown",
             "power_scheme": lambda: {"guid": HIGH_PERFORMANCE, "name": "test"},
             "PROCESSES": 2, "SWEEP": [64], "VARIANTS": ["fetch"]}
    with tempfile.TemporaryDirectory() as directory, mock.patch.dict(globals(), stubs), \
            mock.patch("shutil.which", return_value="nvidia-smi"), \
            mock.patch("time.sleep") as sleep, contextlib.redirect_stdout(io.StringIO()):
        out = Path(directory)
        (out / "correctness").mkdir()
        (out / "correctness" / "summary.json").write_text(
            json.dumps({"passed": True, "adapter": "warp"}))
        (out / "bin").mkdir()
        (out / "bin" / "Harness.exe").write_text("")
        args = argparse.Namespace(out=out, adapter="hardware")
        timing_dir = out / "timing"

        def run_timing() -> int:
            try:
                timing(args)
            except SystemExit as stop:
                return 1 if stop.code else 0
            return 0

        expect(run_timing() == 1 and not state["orders"],
               "timing ran on a correctness run of another adapter")
        (out / "correctness" / "summary.json").write_text(
            json.dumps({"passed": True, "adapter": "hardware"}))
        expect(run_timing() == 0, "the stubbed timing round stopped")
        expect(state["orders"] == [0, 1, 1, 1, 1], f"timing ran orders {state['orders']}")
        kept = read_json(timing_dir / "n64-fetch-p00.json") or {}
        expect(kept.get("thermal") == {"start": [50, 2000], "end": [50, 2000]},
               f"process 0 was not kept with its readings: {kept.get('thermal')}")
        expect((timing_dir / "n64-fetch-p01.notrun.json").exists()
               and not (timing_dir / "n64-fetch-p01.json").exists()
               and all((timing_dir / f"n64-fetch-p01.discarded{a}.json").exists()
                       for a in range(1, ATTEMPTS + 1)),
               "process 1 was not discarded 4 times and marked not run")
        expect(sleep.call_count == ATTEMPTS - 1, f"rested {sleep.call_count} times, not 3")

        expect(run_timing() == 0 and len(state["orders"]) == 5,
               "a resume of a finished round ran processes again")

        (timing_dir / "n64-fetch-p00.json").write_text(json.dumps({"kernels": []}))
        expect(run_timing() == 0 and state["orders"][5:] == [0],
               "a result without thermal readings was kept on resume")
        expect("thermal" in (read_json(timing_dir / "n64-fetch-p00.json") or {})
               and (timing_dir / "n64-fetch-p00.failed.json").exists(),
               "the stale result was not set aside and rerun")

        (timing_dir / "n64-fetch-p00.json").unlink()
        state["exit"] = 1
        expect(run_timing() == 1, "a non-zero exit did not stop the round")
        expect(not (timing_dir / "n64-fetch-p00.json").exists()
               and (timing_dir / "n64-fetch-p00.failed.json").exists(),
               "the failed process's result was not renamed .failed.json")

        # A process stopped after two discards resumes at attempt 3, keeping attempts 1 and 2.
        for attempt in (1, 2):
            (timing_dir / f"n64-fetch-p00.discarded{attempt}.json").write_text('"earlier"')
        state.update(exit=0, low={0, 1})
        calls = len(state["orders"])
        expect(run_timing() == 0 and state["orders"][calls:] == [0, 0],
               f"a resume after two discards ran {state['orders'][calls:]}, not two attempts")
        expect(all(read_json(timing_dir / f"n64-fetch-p00.discarded{a}.json") == "earlier"
                   for a in (1, 2))
               and all((timing_dir / f"n64-fetch-p00.discarded{a}.json").exists()
                       for a in (3, 4))
               and (timing_dir / "n64-fetch-p00.notrun.json").exists(),
               "the resumed attempts overwrote earlier ones or did not mark the process not run")


def selftest(args) -> None:
    counted = count_dxil(CANNED_DUMPBIN)
    expect(counted["instructions"] == 6, f"count_dxil counted {counted['instructions']}, not 6")
    expect(counted["ops"] == {"dx.op.createHandleFromBinding": 1, "dx.op.rawBufferLoad": 2,
                              "dx.op.threadId": 1}, f"count_dxil histogram {counted['ops']}")

    rng = random.Random(1)
    counts = {"F-u-fetch": {"instructions": 164}, "H1-u-fetch": {"instructions": 185}}

    def decide_for(runs, not_run=()):
        return analyze_runs(runs, set(not_run), counts)["decision"]

    d = decide_for(synthetic_runs(rng, {}))
    expect(d["verdict"] == "cheap" and not d["escalate"],
           f"H1 equal to F within noise gave {d['verdict']}, escalate {d['escalate']}")
    d = decide_for(synthetic_runs(rng, {"H1-u": 1.10}))
    expect(d["verdict"] == "costly" and d["slowdowns"] == ["uniform"],
           f"H1 uniform 10% slower gave {d['verdict']} {d['slowdowns']}")
    d = decide_for(synthetic_runs(rng, {"H1-w": 0.90}))
    expect(d["verdict"] == "cheap" and d["speedups"] == ["wave-uniform"],
           f"H1 wave-uniform 10% faster gave {d['verdict']} {d['speedups']}")
    d = decide_for(synthetic_runs(rng, {"H2": 2.5}))
    expect(d["escalate"] and d["verdict"] == "cheap",
           f"H2 at 2.5 times F gave {d['verdict']}, escalate {d['escalate']}")
    d = decide_for(synthetic_runs(rng, {"H2-d": 2.5}))
    expect(d["escalate"], "H2 at 2.5 times F in the divergent mode alone did not escalate")
    d = decide_for(synthetic_runs(rng, {"H1-d": 3.5}))
    expect(d["escalate"] and d["verdict"] == "cheap", "H1 divergent at 3.5 times F divergent")
    d = decide_for(synthetic_runs(rng, {"H1-d": 2.5}))
    expect(not d["escalate"], "H1 divergent at 2.5 times F divergent escalated")
    d = decide_for(synthetic_runs(rng, {"H2": 1.95}))
    expect(not d["escalate"], "H2 at 1.95 times F escalated")
    d = decide_for([])
    expect(d["verdict"] == "incomplete", f"a missing judged configuration gave {d['verdict']}")
    d = decide_for(synthetic_runs(rng, {}, drop=("H1-w",)))
    expect(d["verdict"] == "incomplete", f"a missing H1 wave-uniform gave {d['verdict']}")
    d = decide_for(synthetic_runs(rng, {}, drop=("F-w",)))
    expect(d["verdict"] == "incomplete", f"a missing F wave-uniform gave {d['verdict']}")
    d = decide_for(synthetic_runs(rng, {}), not_run=[JUDGED])
    expect(d["verdict"] == "incomplete", f"a judged configuration marked not run gave {d['verdict']}")
    d = decide_for(synthetic_runs(rng, {}, variant="alu"))
    expect(d["verdict"] == "incomplete", "only an alu configuration gave a verdict")

    def judged(summary: dict, mode: str, label: str) -> dict:
        return summary["configurations"][f"n{JUDGED[0]}-{JUDGED[1]}"]["modes"][mode][
            "comparisons"][label]

    summary = analyze_runs(synthetic_runs(random.Random(11), {"H1-u": 1.03}), set(), counts)
    c = judged(summary, "uniform", "H1-F")
    expect(c["low"] > 0 and not c["slowdown"] and summary["decision"]["verdict"] == "cheap",
           f"H1 uniform 3% slower with 1% noise: interval [{c['low']:.1f}, {c['high']:.1f}], "
           f"slowdown {c['slowdown']}, verdict {summary['decision']['verdict']}")
    summary = analyze_runs(synthetic_runs(random.Random(12), {"H1-u": 1.10},
                                          noise={"H1-u": 0.6}), set(), counts)
    c = judged(summary, "uniform", "H1-F")
    expect(c["low"] <= 0 <= c["high"] and c["relative"] > BUDGET
           and summary["decision"]["verdict"] == "cheap",
           f"H1 uniform with a large, noisy delta: {c['relative']:+.1%} in [{c['low']:.1f}, "
           f"{c['high']:.1f}], verdict {summary['decision']['verdict']}")
    summary = analyze_runs(synthetic_runs(random.Random(13), {}), set(), counts)
    c = judged(summary, "uniform", "H1m-H1")
    expect(c["slowdown"] and 0.45 < c["relative"] < 0.55
           and summary["decision"]["verdict"] == "cheap",
           f"H1m at 1.5 times H1 gave H1m - H1 {c['relative']:+.1%}, slowdown {c['slowdown']}, "
           f"verdict {summary['decision']['verdict']}")

    selftest_timing()

    first = analyze_runs(synthetic_runs(random.Random(7), {"H1-u": 1.02}), set(), counts)
    second = analyze_runs(synthetic_runs(random.Random(7), {"H1-u": 1.02}), set(), counts)
    expect(first == second, "the analysis is not deterministic")
    expect(f"n{JUDGED[0]}-{JUDGED[1]}" not in first["notRun"] and "n64-fetch" in first["notRun"],
           "the not run list is wrong")
    expect(first["configurations"]["n1048576-fetch"]["dxilInstructions"]["F-u"] == 164,
           "the DXIL counts did not reach the configuration")

    def run(started, ended, ac=(True, True)):
        return {"powerOnAcStart": ac[0], "powerOnAcEnd": ac[1]}, started, ended

    clocks = [2000, 2000, 2000]
    run_json, a, b = run((50, 2000), (60, 1900))
    expect(discard_reason(run_json, a, b, clocks) is None, "a 10 C rise was discarded")
    run_json, a, b = run((50, 2000), (61, 2000))
    expect(discard_reason(run_json, a, b, clocks), "an 11 C rise was kept")
    run_json, a, b = run((50, 2000), (50, 1899))
    expect(discard_reason(run_json, a, b, clocks), "a clock below 95% of the median was kept")
    run_json, a, b = run((50, 2000), (50, 1900))
    expect(discard_reason(run_json, a, b, clocks) is None, "a clock at 95% was discarded")
    run_json, a, b = run((50, 2000), (50, 2000), ac=(True, False))
    expect(discard_reason(run_json, a, b, clocks), "a process off AC at its end was kept")
    run_json, a, b = run((50, 2000), (50, 2000), ac=(False, True))
    expect(discard_reason(run_json, a, b, clocks), "a process off AC at its start was kept")
    run_json, _, _ = run(None, None)
    expect(discard_reason(run_json, None, None, []) is None, "a process without a vendor tool "
                                                              "was discarded")

    with tempfile.TemporaryDirectory() as directory:
        out = Path(directory)
        (out / "timing").mkdir()
        (out / "kernels").mkdir()
        (out / "kernels" / "counts.json").write_text(json.dumps(counts))
        for run_json in synthetic_runs(random.Random(3), {}):
            name = f"n{run_json['threads']}-{run_json['variant']}-p{run_json['process']:02d}.json"
            run_json["thermal"] = {"start": None, "end": None}
            (out / "timing" / name).write_text(json.dumps(run_json))
        (out / "timing" / "n64-fetch-p00.discarded1.json").write_text("{}")
        with contextlib.redirect_stdout(io.StringIO()):
            analyze(argparse.Namespace(out=out))
        summary = json.loads((out / "summary.json").read_text())
        expect(summary["decision"]["verdict"] == "cheap", "analyze on files did not give cheap")
        sums = (out / "SHA256SUMS").read_text()
        expect("analysis.txt" in sums and "timing/n1048576-fetch-p19.json" in sums,
               "SHA256SUMS misses files")
    print("selftest: passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["build", "correctness", "timing", "analyze", "selftest"])
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "spike-1")
    parser.add_argument("--deps", type=Path, default=ROOT / "build" / "spike-1-deps")
    parser.add_argument("--adapter", choices=["hardware", "warp"], default="hardware")
    args = parser.parse_args()
    {"build": build, "correctness": correctness, "timing": timing, "analyze": analyze,
     "selftest": selftest}[args.command](args)


if __name__ == "__main__":
    main()
