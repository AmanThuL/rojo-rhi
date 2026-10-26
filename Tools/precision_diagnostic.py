"""Bind the existing Debug GBV precision test's inputs and inspect its ordinary CLI results.

This temporary diagnostic never starts a process. The workflow invokes the original test directly;
this tool retains a source-bound own-process module observation and disk identities, not memory
code, active-device vtable ownership or hardware acceptance.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path
import re
import shutil
import sys
import xml.etree.ElementTree as ET

try:
    from .shader_library import LibraryError, abi_version, check_dxil
except ImportError:
    from shader_library import LibraryError, abi_version, check_dxil


CASE = "AbiPrecise preserves separate dynamic fp32 multiply and add"
DXIL_SHA = "2c6db40278e930da0749c2832ac7276b96c75942a2af6320f16aefbe03da2b5b"
WARP_SHA = "2a08692cba4c130593329255627fb915d666c90cda53d284594bc8438fd4f49d"
TEST_DIR = Path("build/windows/x64/debug/rojorhi-test")
OUTPUT = Path("build/precision-focused")
DLLS = {
    "D3D12/D3D12Core.dll": "ThirdParty/agility/bin/D3D12Core.dll",
    "D3D12/d3d12SDKLayers.dll": "ThirdParty/agility/bin/d3d12SDKLayers.dll",
    "d3d10warp.dll": "ThirdParty/warp/bin/d3d10warp.dll",
    "amd_fidelityfx_loader_dx12.dll": "ThirdParty/fidelityfx/bin/amd_fidelityfx_loader_dx12.dll",
    "amd_fidelityfx_upscaler_dx12.dll": "ThirdParty/fidelityfx/bin/amd_fidelityfx_upscaler_dx12.dll",
}
SOURCES = (
    "Tests/Conformance/PrecisionConformanceTests.cpp", "Tests/Support/D3D12/D3D12Backend.cpp",
    "Tests/Support/GpuKit.cpp", "Shaders/Tests/AbiPrecise.slang", "Shaders/Tests/Modules/PreciseTest.slang",
    "Shaders/RojoRHI/RojoRHI.slang", "xmake/slang.lua", "xmake/targets.lua", "xmake/setup.lua",
    "Tools/precision_diagnostic.py", "Tools/shader_library.py", ".github/workflows/ci.yml",
    "Tests/D3D12/PrecisionModuleDiagnostics.cpp",
)
MODULE_NAMES = frozenset({"d3d12.dll", "d3d12core.dll", "d3d12sdklayers.dll", "d3d10warp.dll",
                          "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"})


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_library(container, dxil, abi):
    require(len(dxil) == 3420 and digest(dxil) == DXIL_SHA, "original signed DXIL changed")
    check_dxil("preciseMain", dxil)
    header = (f"rojorhi.d3d12.shaderlibrary\nabi {abi}\n"
              "entry preciseMain compute 1 1 1 3420\ndxil 3420\n").encode("ascii")
    require(container == header + dxil, "precision library does not hold exactly the original entry and DXIL")
    return digest(dxil)


def validate_result(xml, inventory, invocation):
    require([line.strip() for line in inventory.splitlines() if line.strip()] == [CASE], "exactly the original case must be listed")
    require(type(invocation.get("inventoryExit")) is int and invocation["inventoryExit"] == 0,
            "inventory did not complete with exit0")
    require(type(invocation.get("testExit")) is int, "actual test terminal status is missing")
    require(invocation.get("case") == CASE, "invoked case differs")
    try:
        root = ET.fromstring(xml)
    except ET.ParseError as error:
        raise ValueError("missing or incomplete Catch2 XML") from error
    cases = root.findall(".//TestCase")
    require(root.tag == "Catch2TestRun" and len(cases) == 1 and cases[0].get("name") == CASE,
            "XML does not contain exactly the original case")
    assertions = root.find("OverallResults")
    counts = root.find("OverallResultsCases")
    require(len(root.findall("OverallResults")) == 1 and len(root.findall("OverallResultsCases")) == 1,
            "completed XML totals are missing or repeated")
    successes, failures = (int(assertions.get(key, "-1")) for key in ("successes", "failures"))
    require(successes >= 0 and failures >= 0 and successes + failures == 8
            and int(assertions.get("expectedFailures", "-1")) == 0,
            "the original eight assertions did not all execute")
    require(int(assertions.get("skips", "-1")) == 0
            and int(counts.get("skips", "-1")) == 0 and int(counts.get("expectedFailures", "-1")) == 0
            and int(counts.get("successes", "-1")) == int(failures == 0)
            and int(counts.get("failures", "-1")) == int(failures != 0), "case totals differ or the case skipped")
    require(cases[0].find("OverallResult") is not None
            and cases[0].find("OverallResult").get("success") == str(failures == 0).lower()
            and cases[0].find("OverallResult").get("skips") == "0",
            "case outcome differs from assertion totals")
    require(invocation["testExit"] == (0 if failures == 0 else 42), "test exit differs from the Catch2 outcome")
    return {"passed": failures == 0, "assertions": 8, "successes": successes, "failures": failures}


def compare_closure(before, after):
    return [key for key in ("files", "versions") if before.get(key) != after.get(key)]


def canonical_path(value):
    require(type(value) is str and value and "\x00" not in value, "invalid observed path")
    if ntpath.splitdrive(value)[0] and ntpath.isabs(value):
        return ntpath.normcase(ntpath.normpath(value))
    require(Path(value).is_absolute(), "observed path is not absolute")
    return str(Path(value))


def image_name(value):
    return ntpath.basename(value.replace("/", "\\")).lower()


def validate_mapped_snapshot(value, executable):
    require(type(value) is dict and type(value.get("schemaVersion")) is int and value["schemaVersion"] == 1,
            "mapped-image schema differs")
    require(value.get("complete") is True and value.get("errors") == [], "mapped-image capture is incomplete")
    require(value.get("case") == CASE and value.get("phase") == "assertionStarting"
            and type(value.get("sourceLine")) is int and value["sourceLine"] == 75
            and type(value.get("sourceFile")) is str
            and ntpath.basename(value["sourceFile"].replace("/", "\\")) == "PrecisionConformanceTests.cpp"
            and value.get("expression") == "resultBits == 0u", "mapped-image source point differs")
    require(type(value.get("processId")) is int and 0 < value["processId"] < 2**32
            and type(value.get("pointerBytes")) is int and value["pointerBytes"] == 8, "mapped-image process metadata differs")
    expected_exe = canonical_path(executable)
    require(canonical_path(value.get("executablePath")) == expected_exe, "observed executable differs")
    snapshots = value.get("snapshots")
    require(type(snapshots) is list and len(snapshots) == 2, "two complete enumeration observations are required")
    identities = []
    for snapshot in snapshots:
        require(type(snapshot) is dict and snapshot.get("complete") is True and snapshot.get("errors") == [],
                "module enumeration failed")
        modules, attempts = snapshot.get("modules"), snapshot.get("attempts")
        require(type(modules) is list and 1 <= len(modules) <= 4096 and type(attempts) is list and 1 <= len(attempts) <= 3,
                "module enumeration metadata is missing")
        for attempt in attempts:
            require(type(attempt) is dict and attempt.get("success") is True
                    and type(attempt.get("error")) is int and attempt["error"] == 0
                    and type(attempt.get("capacityBytes")) is int and 0 < attempt["capacityBytes"] <= 4096 * 8
                    and type(attempt.get("neededBytes")) is int and attempt["neededBytes"] > 0,
                    "module enumeration attempt failed")
        require(attempts[-1]["neededBytes"] <= attempts[-1]["capacityBytes"]
                and attempts[-1]["neededBytes"] == len(modules) * 8, "module enumeration is truncated")
        current, bases = [], set()
        for module in modules:
            require(type(module) is dict and type(module.get("base")) is str
                    and re.fullmatch(r"0x[0-9a-fA-F]+", module["base"]), "invalid module base")
            address = int(module["base"], 16)
            require(0 < address < 2**64 and address not in bases, "module instance is zero or repeated")
            bases.add(address)
            require(type(module.get("size")) is int and 0 < module["size"] < 2**32, "invalid mapped image size")
            path = canonical_path(module.get("path"))
            require(type(module.get("pathCharacters")) is int
                    and module["pathCharacters"] == len(module["path"].encode("utf-16-le")) // 2
                    and type(module.get("pathCapacity")) is int
                    and 0 < module["pathCharacters"] < module["pathCapacity"] <= 32768, "mapped path is truncated")
            current.append((address, path, module["size"]))
        require(any(path == expected_exe for _, path, _ in current), "executable mapping is absent")
        identities.append(sorted(current))
    require(identities[0] == identities[1], "module enumeration changed between observations")
    relevant = [module for module in snapshots[-1]["modules"] if image_name(module["path"]) in MODULE_NAMES]
    names = {image_name(module["path"]) for module in relevant}
    require({"d3d12.dll", "d3d12core.dll", "d3d10warp.dll"} <= names, "required runtime mapping is absent")
    warp = [module for module in relevant if image_name(module["path"]) == "d3d10warp.dll"]
    expected_warp = canonical_path(ntpath.join(ntpath.dirname(executable), "d3d10warp.dll")
                                   if ntpath.splitdrive(executable)[0] else str(Path(executable).parent / "d3d10warp.dll"))
    expected_core = canonical_path(ntpath.join(ntpath.dirname(executable), "D3D12", "D3D12Core.dll")
                                   if ntpath.splitdrive(executable)[0] else str(Path(executable).parent / "D3D12/D3D12Core.dll"))
    return {"relevantImages": relevant, "processId": value["processId"],
            "expectedUniqueWarp": len(warp) == 1 and canonical_path(warp[0]["path"]) == expected_warp,
            "expectedPrivateCoreMapped": any(canonical_path(image["path"]) == expected_core for image in relevant)}


def mapped_identity(raw, executable, observed_versions, read_bytes=None, frozen_files=None):
    observation = validate_mapped_snapshot(raw, executable)
    paths = {image["path"] for image in observation["relevantImages"]}
    require(type(observed_versions) is dict and set(observed_versions) == paths, "mapped DLL version inventory differs")
    require(all(type(version) is str and re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", version)
                for version in observed_versions.values()), "mapped DLL version is incomplete")
    read_bytes = read_bytes or (lambda path: Path(path).read_bytes())
    frozen_files = frozen_files or {}
    files, errors = {}, []
    for path in sorted(paths):
        try:
            data = read_bytes(path)
            require(type(data) is bytes and bool(data), "mapped disk image is empty")
            files[path] = {"size": len(data), "sha256": digest(data), "version": observed_versions[path]}
            frozen = frozen_files.get(canonical_path(path))
            files[path]["binding"] = "frozen-input" if frozen is not None else "after-only"
            if frozen is not None:
                require({key: files[path][key] for key in ("size", "sha256")} == frozen,
                        "observed mapped path differs from its frozen input")
        except (OSError, ValueError) as error:
            errors.append({"path": path, "errorType": type(error).__name__, "message": str(error)})
    warp = [image for image in observation["relevantImages"] if image_name(image["path"]) == "d3d10warp.dll"]
    expected = (observation["expectedUniqueWarp"] and observation["expectedPrivateCoreMapped"] and len(warp) == 1
                and files.get(warp[0]["path"], {}).get("sha256") == WARP_SHA
                and files.get(warp[0]["path"], {}).get("version") == "1.0.20.0")
    return {"complete": not errors, "expectedDeployment": expected and not errors, "errors": errors,
            "expectedPrivateCoreMapped": observation["expectedPrivateCoreMapped"],
            "images": observation["relevantImages"], "files": files, "processId": observation["processId"],
            "identityScope": "mapped paths at the assertion plus disk identities read afterward; not active-Core, JIT or memory code"}


def verify_config(text):
    expected = {"mode": "debug", "rojorhi_backend": "d3d12", "rojorhi_d3d12_adapter": "warp", "rojorhi_d3d12_debug": "gbv"}
    for key, value in expected.items():
        require(re.findall(r"^\s*" + key + r'\s*=\s*"([^"]+)"\s*,?\s*$', text, re.MULTILINE) == [value],
                f"pre-test configuration is not exactly {key}={value}")
    return expected


def collect_files(root):
    directory = root / TEST_DIR
    pdbs = sorted(directory.glob("*.pdb"))
    require(bool(pdbs), "Debug PDB is missing")
    paths = [TEST_DIR / "RojoRHITests.exe", TEST_DIR / "Shaders/Standard.d3d12lib",
             TEST_DIR / "Shaders/AbiPrecise.d3d12lib", TEST_DIR / "Shaders/AbiPrecise.dxil"]
    paths += [p.relative_to(root) for p in pdbs]
    paths += [TEST_DIR / name for name in DLLS]
    paths += [Path(name) for name in DLLS.values()]
    paths += [Path(name) for name in SOURCES]
    files = {}
    for relative in paths:
        path = root / relative
        require(path.is_file() and not path.is_symlink(), f"required input is missing or a symlink: {relative}")
        data = path.read_bytes()
        require(bool(data), f"required input is empty: {relative}")
        files[relative.as_posix()] = {"size": len(data), "sha256": digest(data)}
    for name, source in DLLS.items():
        require(files[(TEST_DIR / name).as_posix()] == files[source], f"runtime copy differs from setup: {name}")
    require(files[(TEST_DIR / "d3d10warp.dll").as_posix()]["sha256"] == WARP_SHA, "WARP20 pin differs")
    verify_library((directory / "Shaders/AbiPrecise.d3d12lib").read_bytes(),
                   (directory / "Shaders/AbiPrecise.dxil").read_bytes(),
                   abi_version((root / "Shaders/RojoRHI/RojoRHI.slang").read_text()))
    return files


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def versions(output, position, files):
    value = json.loads((output / f"versions-{position}.json").read_text(encoding="utf-8-sig"))
    require(set(value) == {name for name in files if name.endswith(".dll")}, "DLL version inventory differs")
    require(all(isinstance(item, str) and re.fullmatch(r"[0-9]+(?:\.[0-9]+){3}", item)
                for item in value.values()), "DLL version is incomplete")
    return value


def before(root, output):
    output.mkdir(parents=True, exist_ok=True)
    config = (root / ".xmake/windows/x64/xmake.conf").read_bytes()
    settings = verify_config(config.decode("utf-8"))
    files = collect_files(root)
    for name, identity in files.items():
        destination = output / "inputs" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        require(not destination.exists(), "input archive already exists")
        shutil.copyfile(root / name, destination)
        require(digest(destination.read_bytes()) == identity["sha256"], "archived input differs")
    (output / "debug-config.conf").write_bytes(config)
    write_json(output / "before.json", {"files": files, "configuration": settings, "configSHA256": digest(config),
               "workflowSHA": os.environ.get("GITHUB_SHA"), "runId": os.environ.get("GITHUB_RUN_ID"),
               "case": CASE, "identityScope": "disk only; not a loaded-module census"})


def checked_closure(root, output, position="check"):
    require(not any((output / name).exists() for name in ("before-error.json", "check-error.json")),
            "a prior diagnostic preparation failed")
    initial = json.loads((output / "before.json").read_text())
    final = {"files": collect_files(root), "versions": versions(output, "before", initial["files"])}
    write_json(output / f"{position}-closure.json", final)
    for name, identity in initial["files"].items():
        require(digest((output / "inputs" / name).read_bytes()) == identity["sha256"], "archived input changed")
    require(initial["files"] == final["files"], "Debug GBV inputs changed since the pre-test snapshot")
    return initial, final


def after(root, output):
    require(not any((output / name).exists() for name in ("powershell-error.txt", "versions-error.txt")),
            "the ordinary workflow invocation failed")
    for name in ("inventory.stdout", "inventory.stderr", "test.stdout", "test.stderr"):
        require((output / name).is_file(), "redirected process output is missing: " + name)
    _, final = checked_closure(root, output, "after")
    initial = dict(final)
    final["versions"] = versions(output, "after", final["files"])
    write_json(output / "after.json", final)
    changed = compare_closure(initial, final)
    require(not changed, "diagnostic input closure changed: " + ", ".join(changed))
    invocation = json.loads((output / "invocation.json").read_text(encoding="utf-8-sig"))
    outcome = validate_result((output / "result.xml").read_bytes(),
                              (output / "inventory.stdout").read_text(encoding="utf-8-sig"), invocation)
    try:
        require(b"precision mapped-image observation:" not in (output / "test.stderr").read_bytes(),
                "mapped-image observer reported failure; see test.stderr")
        require(invocation.get("moduleObservationRequested") is True
                and invocation.get("moduleObservationPath") == str((output / "mapped-modules.json").resolve()),
                "mapped-image observation was not requested at its fixed output")
        require(not (output / "mapped-versions-error.txt").exists(), "mapped-image disk version collection failed")
        frozen = {canonical_path(str((root / name).resolve())): value for name, value in final["files"].items()}
        identity = mapped_identity(json.loads((output / "mapped-modules.json").read_text(encoding="utf-8")),
                                   str((root / TEST_DIR / "RojoRHITests.exe").resolve()),
                                   json.loads((output / "mapped-versions.json").read_text(encoding="utf-8-sig")),
                                   frozen_files=frozen)
    except (OSError, ValueError) as error:
        identity = {"complete": False, "expectedDeployment": False,
                    "errors": [{"errorType": type(error).__name__, "message": str(error)}]}
    write_json(output / "mapped-after.json", identity)
    passed = outcome["passed"] and identity["complete"] and identity["expectedDeployment"]
    write_json(output / "result.json", {"case": CASE, "invocation": invocation, "outcome": outcome,
               "numericalComplete": True, "diagnosticPassed": passed, "mappedIdentity": identity,
               "diskInputsUnchanged": True, "normalGateAcceptance": False,
               "identityScope": "source-bound mapped-image observation and separate disk identities"})
    return 0 if passed else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("phase", choices=("before", "check", "after"))
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    output = root / OUTPUT
    try:
        require("ROJORHI_DEATH_CHILD" not in os.environ, "the diagnostic must run as an ordinary parent")
        if args.phase == "before":
            before(root, output)
        elif args.phase == "check":
            checked_closure(root, output)
        else:
            return after(root, output)
    except (OSError, ValueError, LibraryError) as error:
        output.mkdir(parents=True, exist_ok=True)
        write_json(output / f"{args.phase}-error.json", {"passed": False, "phase": args.phase,
                   "errorType": type(error).__name__, "message": str(error), "normalGateAcceptance": False})
        print(f"precision_diagnostic: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
