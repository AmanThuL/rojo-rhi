#!/usr/bin/env python3
"""Run one GPU command and record its output as evidence outside the published tree.

Hosted CI has no Metal 4 device, so GPU conformance and benchmark runs happen on local hardware and
their raw output is kept beside the repository, never inside it. One call runs one command, saves
its combined output as `<label>.txt` in `<root>/<topic>/<date>/`, and rewrites `environment.json`
and `SHA256SUMS` for the whole directory. The exit code is the command's, so a failing run still
leaves its log behind.

    python3 Tools/gpu_evidence.py --topic v1-gpu-conformance --label gpu \\
        --cwd build/macosx/arm64/debug/rojorhi-legacy-test \\
        -- env MTL_DEBUG_LAYER=1 ./RojoRHILegacyTests "[gpu]~[.]"
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import platform
import shlex
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main_checkout(root: Path) -> Path:
    """The main worktree of the repository holding root, or root when git cannot say."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
            cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
    except (OSError, subprocess.CalledProcessError):
        return root
    return Path(result.stdout.strip()).parent


DEFAULT_EVIDENCE_ROOT = main_checkout(ROOT).parent / "rojo-rhi-evidence"
ENVIRONMENT_FILE = "environment.json"
CHECKSUM_FILE = "SHA256SUMS"


def probe(*command: str) -> str:
    """Return the first line a tool prints, or "unknown" when the tool is missing or fails."""
    try:
        result = subprocess.run(
            command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
    except OSError:
        return "unknown"
    lines = result.stdout.strip().splitlines()
    return lines[0].strip() if result.returncode == 0 and lines else "unknown"


def dirty() -> str:
    """Return "yes" when the checkout has changes, "no" when clean, "unknown" without git."""
    try:
        result = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError:
        return "unknown"
    if result.returncode:
        return "unknown"
    return "yes" if result.stdout.strip() else "no"


def environment() -> dict[str, str]:
    """Describe the machine, toolchain and checkout the evidence was produced with."""
    return {
        "machine": platform.machine(),
        "device": probe("sysctl", "-n", "machdep.cpu.brand_string"),
        "osVersion": probe("sw_vers", "-productVersion"),
        "osBuild": probe("sw_vers", "-buildVersion"),
        "xcode": " ".join(probe("xcodebuild", "-version").split()),
        "metal": probe("xcrun", "metal", "--version"),
        "clang": probe("clang", "--version"),
        "metalCpp": probe("git", "-C", "ThirdParty/metal-cpp", "rev-parse", "HEAD"),
        "commit": probe("git", "rev-parse", "HEAD"),
        "dirty": dirty(),
    }


def write_checksums(directory: Path, recursive: bool = False) -> None:
    """Write one `<sha256>  <name>` line per file of the directory, sorted by name.

    With `recursive`, files at any depth are listed, named by their POSIX path relative to the
    directory.
    """
    lines = []
    paths = directory.rglob("*") if recursive else directory.iterdir()
    entries = sorted((path.relative_to(directory).as_posix(), path) for path in paths)
    for name, path in entries:
        if path.is_file() and name != CHECKSUM_FILE:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            lines.append(f"{digest}  {name}\n")
    (directory / CHECKSUM_FILE).write_text("".join(lines))


def record(directory: Path, label: str, command: list[str], cwd: Path) -> int:
    """Run the command, save its log and the directory's environment and checksums."""
    directory.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    header = f"$ (cd {cwd.relative_to(ROOT) if cwd.is_relative_to(ROOT) else cwd}) " \
             f"{shlex.join(command)}\n"
    (directory / f"{label}.txt").write_text(header + result.stdout + f"exit={result.returncode}\n")
    (directory / ENVIRONMENT_FILE).write_text(json.dumps(environment(), indent=2) + "\n")
    write_checksums(directory, recursive=True)
    return result.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=DEFAULT_EVIDENCE_ROOT,
                        help="evidence root; defaults to ../rojo-rhi-evidence")
    parser.add_argument("--topic", required=True, help="directory under the root, such as t1")
    parser.add_argument("--date", default=datetime.date.today().isoformat())
    parser.add_argument("--label", required=True, help="log name, without .txt")
    parser.add_argument("--cwd", type=Path, default=ROOT, help="directory to run the command in")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- command and arguments")
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command given after --")
    cwd = args.cwd if args.cwd.is_absolute() else ROOT / args.cwd
    directory = args.root / args.topic / args.date
    code = record(directory, args.label, command, cwd)
    print(f"{directory / (args.label + '.txt')}: exit {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
