#!/usr/bin/env python3
"""Check the DXIL the build compiled for the module's heap fetches and raw accesses.

`Shaders/Tests/AbiNonUniform.slang` has one kernel per heap fetch kind (`load`, `store`, texture,
write-only texture, sampler), each indexing the heap through `rojoRHI::nonUniform`. Every heap
descriptor handle in their DXIL must carry the non-uniform flag, and each kernel must still hold a
handle of its kind. `Shaders/Tests/AbiRawAccess.slang` reads two fields of a flat root, loads one
`uint4` and stores one: its DXIL must load the root exactly twice and make the load and the store
one full-width raw access each, with nothing spilled to a local array. The check disassembles each
file with `dxc -dumpbin` and fails, rather than skips, when a file, the shader directory or `dxc`
is missing.

    xmake build RojoRHITests && python3 Tools/check_dxil.py [--shaders DIR] [--dxc PATH]
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# DXIL file -> the resource kind its kernel fetches, as dumpbin annotates the handle.
NONUNIFORM: dict[str, str] = {
    "AbiNonUniform.loadMain.dxil": "ByteAddressBuffer",
    "AbiNonUniform.storeMain.dxil": "RWByteAddressBuffer",
    "AbiNonUniform.textureMain.dxil": "Texture2D",
    "AbiNonUniform.wTextureMain.dxil": "RWTexture2D",
    "AbiNonUniform.samplerMain.dxil": "SamplerState",
}
RAW_ACCESS = "AbiRawAccess.dxil"
ROOT_FIELDS_READ = 2  # AbiRawAccess reads root.input and root.output
FULL_WIDTH_MASK = 15  # four 32-bit components in one rawBufferLoad or rawBufferStore

VALUE = r'%(?:"[^"]*"|[-\w.$]+)'
FROM_BINDING = re.compile(rf"^\s*({VALUE})\s*=\s*call %dx\.types\.Handle @dx\.op\.createHandleFromBinding\(")
FROM_HEAP = re.compile(
    rf"^\s*({VALUE})\s*=\s*call %dx\.types\.Handle @dx\.op\.createHandleFromHeap\("
    rf"i32 218, i32 [^,]+, i1 (true|false), i1 (true|false)\)"
)
ANNOTATE = re.compile(
    rf"^\s*({VALUE})\s*=\s*call %dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle ({VALUE}),"
)
RESOURCE = re.compile(r";.*\bresource:\s*(\w+)")
RAW = re.compile(
    rf"@dx\.op\.rawBuffer(Load|Store)\.\w+\(i32 1(?:39|40), %dx\.types\.Handle ({VALUE}),"
    rf".*\bi8 (\d+), i32 \d+\)"
)
ALLOCA = re.compile(r"=\s*alloca\b")


@dataclass(frozen=True)
class Handle:
    source: str  # "binding" (createHandleFromBinding: the root SRV) or "heap"
    sampler_heap: bool
    non_uniform: bool
    resource: str  # the base type dumpbin names, "Texture2D" for "Texture2D<4xF32>"; "" if not annotated


@dataclass(frozen=True)
class Access:
    op: str  # "load" or "store"
    handle: Handle
    mask: int


def instruction_lines(disassembly: str) -> list[str]:
    """Return the lines of the listing, without line-ending differences and without declarations."""
    lines = disassembly.replace("\r\n", "\n").split("\n")
    return [line for line in lines if not line.lstrip().startswith("declare ")]


def parse_handles(disassembly: str) -> dict[str, Handle]:
    """Map each handle's SSA name to its origin; an annotated handle keeps its operand's origin."""
    handles: dict[str, Handle] = {}
    for line in instruction_lines(disassembly):
        if match := FROM_BINDING.match(line):
            handles[match.group(1)] = Handle("binding", False, False, "")
        elif match := FROM_HEAP.match(line):
            handles[match.group(1)] = Handle("heap", match.group(2) == "true", match.group(3) == "true", "")
        elif match := ANNOTATE.match(line):
            operand = handles.get(match.group(2), Handle("", False, False, ""))
            resource = RESOURCE.search(line)
            handles[match.group(1)] = Handle(operand.source, operand.sampler_heap, operand.non_uniform,
                                             resource.group(1) if resource else "")
    return handles


def raw_accesses(disassembly: str) -> list[Access]:
    """Return every rawBufferLoad and rawBufferStore call, with its handle and component mask."""
    handles = parse_handles(disassembly)
    accesses: list[Access] = []
    for line in instruction_lines(disassembly):
        if match := RAW.search(line):
            handle = handles.get(match.group(2), Handle("", False, False, ""))
            accesses.append(Access(match.group(1).lower(), handle, int(match.group(3))))
    return accesses


def nonuniform_errors(name: str, disassembly: str, resource: str) -> list[str]:
    """Return the heap handles that lack the non-uniform flag, and a missing handle of the kind."""
    handles = parse_handles(disassembly)
    annotated = {match.group(2) for line in instruction_lines(disassembly) if (match := ANNOTATE.match(line))}
    # Each heap handle once: through its annotation when it has one, which names the resource.
    heap = [(ssa, h) for ssa, h in handles.items() if h.source == "heap" and ssa not in annotated]
    errors = [
        f"{name}: heap handle {ssa} ({h.resource or 'not annotated'}) lacks the non-uniform flag"
        for ssa, h in heap if not h.non_uniform
    ]
    wanted = [h for _, h in heap if h.resource == resource and (resource != "SamplerState" or h.sampler_heap)]
    if not wanted:
        found = sorted({f"{h.resource or '?'}{' (sampler heap)' if h.sampler_heap else ''}" for _, h in heap})
        errors.append(f"{name}: no {resource} handle from the "
                      f"{'sampler' if resource == 'SamplerState' else 'resource'} heap; the kernel no longer "
                      f"fetches its kind (heap handles: {', '.join(found) or 'none'})")
    return errors


def raw_access_errors(name: str, disassembly: str) -> list[str]:
    """Return the deviations from two root loads and one full-width heap load and store."""
    errors: list[str] = []
    accesses = raw_accesses(disassembly)
    root_loads = [a for a in accesses if a.handle.source == "binding" and a.op == "load"]
    if len(root_loads) != ROOT_FIELDS_READ:
        errors.append(f"{name}: {len(root_loads)} root loads, expected {ROOT_FIELDS_READ}, one per field read")
    if any(ALLOCA.search(line) for line in instruction_lines(disassembly)):
        errors.append(f"{name}: an alloca spills a value to a local array")
    for op in ("load", "store"):
        masks = [a.mask for a in accesses if a.handle.source == "heap" and a.op == op]
        if masks != [FULL_WIDTH_MASK]:
            errors.append(f"{name}: heap {op}s have masks {masks}, expected one access with mask {FULL_WIDTH_MASK}")
    return errors


def find_shader_dirs(root: Path) -> list[Path]:
    """Return the shader output directories of every configured test build under root."""
    return sorted(root.glob("build/*/*/*/rojorhi-test/Shaders"))


def find_dxc(root: Path) -> str | None:
    """Return the DXC beside the pinned slangc, else the one on PATH."""
    for name in ("dxc.exe", "dxc"):
        candidate = root / "ThirdParty" / "slang" / "bin" / name
        if candidate.is_file():
            return str(candidate)
    return shutil.which("dxc")


def disassemble(dxc: str, dxil: Path) -> str:
    """Return the `dxc -dumpbin` listing of a DXIL container."""
    result = subprocess.run([dxc, "-dumpbin", str(dxil)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{dxc} -dumpbin {dxil} failed: {result.stderr.strip()}")
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--shaders", type=Path, action="append",
                        help="a directory holding the compiled .dxil files (repeatable); "
                             "defaults to every build/*/*/*/rojorhi-test/Shaders")
    parser.add_argument("--dxc", help="the dxc to disassemble with; defaults to the one beside slangc, then PATH")
    args = parser.parse_args(argv)

    directories = args.shaders or find_shader_dirs(ROOT)
    if not directories:
        print("no build/*/*/*/rojorhi-test/Shaders directory; build RojoRHITests first or pass --shaders")
        return 1
    expected = [*NONUNIFORM, RAW_ACCESS]
    for directory in directories:
        for file in expected:
            if not (directory / file).is_file():
                print(f"{directory / file} is missing: the build compiled no DXIL; a DXC must load for Slang")
                return 1

    dxc = args.dxc or find_dxc(ROOT)
    if dxc is None:
        print("no dxc beside ThirdParty/slang/bin/slangc or on PATH; pass --dxc")
        return 1
    try:
        version = subprocess.run([dxc, "--version"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as error:
        print(f"{dxc}: cannot run dxc: {error}")
        return 1
    print(f"dxc: {dxc} ({(version.stdout.strip().splitlines() or ['no version'])[0]})")

    failed = False
    for directory in directories:
        for file in expected:
            path = directory / file
            try:
                listing = disassemble(dxc, path)
            except (OSError, RuntimeError) as error:
                print(f"{path}: cannot disassemble: {error}")
                return 1
            if file == RAW_ACCESS:
                errors = raw_access_errors(str(path), listing)
            else:
                errors = nonuniform_errors(str(path), listing, NONUNIFORM[file])
            for error in errors:
                print(error)
            if not errors:
                print(f"ok {path}")
            failed = failed or bool(errors)
    if failed:
        return 1
    print(f"DXIL checks passed ({len(expected) * len(directories)} files in {len(directories)} "
          f"director{'y' if len(directories) == 1 else 'ies'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
