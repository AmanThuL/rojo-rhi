#!/usr/bin/env python3
"""Measure what including the public header costs a translation unit, against a stored baseline.

ADR 0012 counts the compile time of the public surface as a cost every including translation unit
pays, measured in hosted CI rather than driving a redesign. This tool compiles two one-line
translation units, interleaved: one that includes `<rojoRHI/RHI.h>` and one that includes only the
standard headers it includes. Their difference is the header's own cost, apart from the standard
library's. It reports the median of each, that difference and the preprocessed size of both, and
compares them with `Tools/header_compile_baseline.json`. Hosted runners are noisy, so the
comparison warns and never fails.

    python3 Tools/header_compile_time.py                 # measure and compare
    python3 Tools/header_compile_time.py --write-baseline  # store this host's result
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "Include" / "rojoRHI" / "RHI.h"
BASELINE = ROOT / "Tools" / "header_compile_baseline.json"
STANDARD_INCLUDE = re.compile(r"^#include <([a-z_]+)>$", re.MULTILINE)
# The header's own cost grows past the baseline by this factor before the comparison warns, and its
# time also by WARN_MS, because a runner's noise is several milliseconds.
WARN_FACTOR = 1.5
WARN_MS = 10.0


def standard_includes(header: str) -> list[str]:
    """Return the standard headers the public header includes, in order."""
    return STANDARD_INCLUDE.findall(header)


def host_name() -> str:
    """Name the host the way the baseline file keys it."""
    return {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")


def compiler_commands(source: Path) -> tuple[list[str], list[str]]:
    """Return the syntax-only and the preprocess-only command for the host's compiler."""
    if sys.platform == "win32":
        cl = shutil.which("cl")
        if not cl:
            raise RuntimeError("cl not found; run from a Visual Studio developer environment")
        base = [cl, "/nologo", "/std:c++latest", "/EHsc", f"/I{ROOT / 'Include'}"]
        return base + ["/Zs", str(source)], base + ["/EP", str(source)]
    compiler = os.environ.get("CXX") or ("clang++" if shutil.which("clang++") else "c++")
    base = [compiler, "-std=c++23", f"-I{ROOT / 'Include'}"]
    return base + ["-fsyntax-only", str(source)], base + ["-E", "-P", str(source)]


def time_once(command: list[str]) -> float:
    """Run a compile and return its wall time in milliseconds; a failed compile is an error."""
    start = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    elapsed = (time.perf_counter() - start) * 1000.0
    if result.returncode:
        raise RuntimeError(f"{' '.join(command)} failed:\n{result.stderr}")
    return elapsed


def preprocessed_bytes(command: list[str]) -> int:
    """Return the size of the preprocessed translation unit, without blank lines."""
    output = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                            check=True).stdout
    return sum(len(line) + 1 for line in output.splitlines() if line.strip())


def measure(repetitions: int) -> dict[str, float | int]:
    """Measure both translation units, alternating which compiles first."""
    includes = standard_includes(HEADER.read_text())
    with tempfile.TemporaryDirectory() as name:
        header_tu = Path(name) / "header.cpp"
        floor_tu = Path(name) / "floor.cpp"
        header_tu.write_text("#include <rojoRHI/RHI.h>\n")
        floor_tu.write_text("".join(f"#include <{include}>\n" for include in includes))
        header_compile, header_preprocess = compiler_commands(header_tu)
        floor_compile, floor_preprocess = compiler_commands(floor_tu)
        time_once(header_compile)
        time_once(floor_compile)
        header_ms: list[float] = []
        floor_ms: list[float] = []
        for repetition in range(repetitions):
            pair = [(header_compile, header_ms), (floor_compile, floor_ms)]
            for command, samples in pair if repetition % 2 == 0 else reversed(pair):
                samples.append(time_once(command))
        header_bytes = preprocessed_bytes(header_preprocess)
        floor_bytes = preprocessed_bytes(floor_preprocess)
    header_median = statistics.median(header_ms)
    floor_median = statistics.median(floor_ms)
    return {
        "headerMs": round(header_median, 1),
        "standardMs": round(floor_median, 1),
        "ownMs": round(header_median - floor_median, 1),
        "headerBytes": header_bytes,
        "ownBytes": header_bytes - floor_bytes,
        "repetitions": repetitions,
    }


def compare(result: dict[str, float | int], baseline: dict[str, float | int] | None) -> list[str]:
    """Return a warning per measure that grew past the baseline by more than WARN_FACTOR."""
    if not baseline:
        return []
    warnings = []
    for key, slack in (("ownMs", WARN_MS), ("ownBytes", 0)):
        before, now = baseline.get(key), result[key]
        if not isinstance(before, (int, float)) or before <= 0:
            continue
        if now > before * WARN_FACTOR and now > before + slack:
            warnings.append(f"{key} {now} is over {WARN_FACTOR}x the baseline {before}")
    return warnings


def report(host: str, result: dict[str, float | int], baseline: dict | None) -> str:
    """Format a Markdown table of this run beside the baseline."""
    rows = [("RHI.h translation unit, ms", "headerMs"), ("standard headers alone, ms", "standardMs"),
            ("RHI.h's own cost, ms", "ownMs"), ("preprocessed RHI.h unit, bytes", "headerBytes"),
            ("RHI.h's own preprocessed bytes", "ownBytes")]
    lines = [f"### Public header compile time ({host}, median of {result['repetitions']})", "",
             "| Measure | This run | Baseline |", "|---|---|---|"]
    for title, key in rows:
        before = baseline.get(key, "none") if baseline else "none"
        lines.append(f"| {title} | {result[key]} | {before} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repetitions", type=int, default=15)
    parser.add_argument("--write-baseline", action="store_true",
                        help="store this host's result in the baseline file")
    args = parser.parse_args(argv)
    host = host_name()
    stored = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    result = measure(args.repetitions)
    text = report(host, result, stored.get(host))
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(text)
    for warning in compare(result, stored.get(host)):
        print(f"::warning::public header compile time: {warning}")
    if args.write_baseline:
        stored[host] = result
        BASELINE.write_text(json.dumps(stored, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
