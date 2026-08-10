#!/usr/bin/env python3
"""Compile each public RHI header as the first include in an otherwise empty translation unit."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC_INCLUDE = ROOT / "RHI" / "Include"
HEADERS = tuple(sorted(PUBLIC_INCLUDE.glob("RHI/**/*.h")))


def main() -> int:
    if not HEADERS:
        print("RHI public header check failed (no headers found)", file=sys.stderr)
        return 1

    include_dirs = (PUBLIC_INCLUDE,)
    command = [
        "xcrun",
        "clang++",
        "-std=c++23",
        "-fsyntax-only",
        "-Werror",
        "-x",
        "c++",
        *[argument for directory in include_dirs for argument in ("-I", str(directory))],
        "-",
    ]

    failures: list[tuple[Path, str]] = []
    for header in HEADERS:
        include = header.relative_to(PUBLIC_INCLUDE).as_posix()
        result = subprocess.run(
            command,
            cwd=ROOT,
            input=f'#include "{include}"\n',
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode:
            failures.append((header.relative_to(ROOT), result.stderr.strip()))

    if failures:
        for header, diagnostic in failures:
            print(f"{header}: standalone include failed", file=sys.stderr)
            print(diagnostic, file=sys.stderr)
        print(f"RHI public header check failed ({len(failures)} header(s))", file=sys.stderr)
        return 1

    print(f"RHI public header check passed ({len(HEADERS)} headers compiled standalone)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
