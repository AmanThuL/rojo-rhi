#!/usr/bin/env python3
"""Check that a Release build inlines a library entry into its caller across the library boundary.

ADR 0012 relies on link-time optimization to make a call into RojoRHI a direct call that inlines.
`Tools/LtoProbe/LtoProbe.cpp` calls `Device::capabilities` from a function kept out of line,
`rojorhiLtoProbe`. This check disassembles that function in the linked probe and fails when it
still calls or branches to the entry, which is what a build without link-time optimization emits.

    xmake f -m release -y && xmake build RojoRHILtoProbe && python3 Tools/check_lto_inline.py
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROBE = "rojorhiLtoProbe"
# The entry's mangled name on Itanium (`Device12capabilities`) and MSVC (`capabilities@Device`).
ENTRY = re.compile(r"Device12capabilities|capabilities@Device")
TRANSFER = re.compile(r"\b(?:bl|b|blr|br|call|jmp)\b", re.IGNORECASE)
# objdump prints `<symbol>:` and dumpbin prints `symbol:` at the start of a function.
LABEL = re.compile(r"^(?:[0-9a-fA-F]+ )?<?([^\s<>:]+)>?:\s*$")


def function_body(disassembly: str, name: str) -> list[str]:
    """Return the instruction lines of the named function, empty when the listing lacks it."""
    body: list[str] = []
    inside = False
    for line in disassembly.splitlines():
        label = LABEL.match(line.strip())
        if label:
            symbol = label.group(1).lstrip("_")
            if inside:
                break
            inside = symbol == name
            continue
        if inside and line.strip():
            body.append(line)
    return body


def entry_calls(body: list[str]) -> list[str]:
    """Return the lines of the body that call or branch to the library entry."""
    return [line for line in body if TRANSFER.search(line) and ENTRY.search(line)]


def find_probe(build: Path) -> Path | None:
    """Return the Release probe under the build directory, whatever the platform and architecture."""
    for name in ("RojoRHILtoProbe", "RojoRHILtoProbe.exe"):
        matches = sorted(build.glob(f"*/*/release/rojorhi-lto-probe/{name}"))
        if matches:
            return matches[0]
    return None


def find_dumpbin() -> str | None:
    """Return a dumpbin from PATH or from the newest Visual Studio installation."""
    found = shutil.which("dumpbin")
    if found:
        return found
    root = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    install = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
        text=True, stdout=subprocess.PIPE,
    ).stdout.strip()
    pattern = os.path.join(install, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "dumpbin.exe")
    matches = sorted(glob.glob(pattern))
    return matches[-1] if matches else None


def disassemble(binary: Path) -> str:
    """Disassemble the probe function with the platform's tool."""
    if sys.platform == "darwin":
        command = ["xcrun", "objdump", "-d", "--no-show-raw-insn",
                   f"--disassemble-symbols=_{PROBE}", str(binary)]
    elif sys.platform == "win32":
        dumpbin = find_dumpbin()
        if not dumpbin:
            raise RuntimeError("dumpbin not found; install the MSVC build tools")
        command = [dumpbin, "/nologo", "/disasm:nobytes", str(binary)]
    else:
        command = ["objdump", "-d", "--no-show-raw-insn", f"--disassemble={PROBE}", str(binary)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed: {result.stderr.strip()}")
    return result.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", type=Path, nargs="?",
                        help="the linked RojoRHILtoProbe; defaults to the Release one under build/")
    args = parser.parse_args(argv)
    args.binary = args.binary or find_probe(ROOT / "build")
    if args.binary is None:
        print("no Release RojoRHILtoProbe under build/; run xmake build RojoRHILtoProbe first")
        return 1
    body = function_body(disassemble(args.binary), PROBE)
    if not body:
        print(f"{args.binary}: no disassembly of {PROBE}; is it a probe built with symbols?")
        return 1
    calls = entry_calls(body)
    if calls:
        print(f"{args.binary}: {PROBE} still calls Device::capabilities, so it was not inlined:")
        for line in calls:
            print(f"  {line.strip()}")
        return 1
    print(f"{args.binary}: Device::capabilities is inlined into {PROBE} ({len(body)} instructions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
