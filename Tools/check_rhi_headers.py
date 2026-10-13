#!/usr/bin/env python3
"""Compile each public RHI header as the first include in an otherwise empty translation unit."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
PUBLIC_ROOTS = tuple(COMPONENT / name for name in ("Include", "Legacy/Include"))
HEADERS = tuple((root, header) for root in PUBLIC_ROOTS for header in sorted(root.glob("rojoRHI/**/*.h")))


def main() -> int:
    if not HEADERS:
        print("RHI public header check failed (no headers found)", file=sys.stderr)
        return 1

    failures: list[tuple[Path, str]] = []
    for root, header in HEADERS:
        command = [
            "xcrun",
            "clang++",
            "-std=c++23",
            "-fsyntax-only",
            "-Werror",
            "-x",
            "c++",
            "-I",
            str(root),
            "-",
        ]
        include = header.relative_to(root).as_posix()
        result = subprocess.run(
            command,
            cwd=COMPONENT,
            input=f'#include "{include}"\n',
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode:
            failures.append((header.relative_to(COMPONENT), result.stderr.strip()))

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
