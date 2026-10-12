"""Build and run spike 3; throwaway spike source.

    python3 Tools/Spikes/MultiDrawIndirect/run.py build OUT
    python3 Tools/Spikes/MultiDrawIndirect/run.py correctness OUT
    python3 Tools/Spikes/MultiDrawIndirect/run.py timing OUT
    python3 Tools/Spikes/MultiDrawIndirect/run.py analyze OUT

OUT is an evidence directory outside the repository. The protocol is fixed in
docs/research/2026-10-12-spike-3-multi-draw-indirect.md.
"""
import json
import os
import platform
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
MAX_COUNTS = [1, 16, 256, 4096, 65536]
PROCESSES = 20
RESAMPLES = 10000
BUDGET = 0.05


def sh(argv, **kw):
    return subprocess.run(list(map(str, argv)), text=True, capture_output=True, **kw)


def build(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    steps = [
        ["xcrun", "-sdk", "macosx", "metal", "-std=metal4.0", SOURCE / "Kernels.metal", "-o",
         out / "Kernels.metallib"],
        ["xcrun", "clang++", "-std=c++23", "-O2", "-fblocks", "-Wall", "-Wextra", "-I",
         ROOT / "ThirdParty/metal-cpp", SOURCE / "Harness.cpp", "-framework", "Foundation",
         "-framework", "Metal", "-o", out / "Harness"],
    ]
    for argv in steps:
        result = sh(argv)
        if result.returncode or result.stderr.strip():
            print(result.stdout + result.stderr)
        if result.returncode:
            sys.exit(1)
    environment = {
        "machine": platform.machine(),
        "osBuild": sh(["sw_vers", "-buildVersion"]).stdout.strip(),
        "osVersion": sh(["sw_vers", "-productVersion"]).stdout.strip(),
        "xcode": " ".join(sh(["xcodebuild", "-version"]).stdout.split()),
        "metal": sh(["xcrun", "-sdk", "macosx", "metal", "--version"]).stdout.splitlines()[0],
        "clang": sh(["xcrun", "clang++", "--version"]).stdout.splitlines()[0],
        "metalCpp": sh(["git", "-C", ROOT / "ThirdParty/metal-cpp", "rev-parse", "HEAD"])
        .stdout.strip(),
        "spikeCommit": sh(["git", "-C", ROOT, "rev-parse", "HEAD"]).stdout.strip(),
        "limits": json.loads(sh([out / "Harness", "limits", out / "Kernels.metallib"],
                                env=validation_env()).stdout),
    }
    (out / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    print(json.dumps(environment, indent=2))


def validation_env(shader: bool = True) -> dict:
    env = os.environ.copy()
    env["MTL_DEBUG_LAYER"] = "1"
    if shader:
        env["MTL_SHADER_VALIDATION"] = "1"
    else:
        env.pop("MTL_SHADER_VALIDATION", None)
    return env


def timing_env() -> dict:
    env = os.environ.copy()
    for key in [k for k in env if k.startswith(("MTL_", "METAL_"))]:
        del env[key]
    return env


def power_source() -> str:
    first = sh(["pmset", "-g", "batt"]).stdout.splitlines()[:1]
    return first[0].split("'")[1] if first and "'" in first[0] else "unknown"


def harness(out: Path, max_count: int, order: int, mode: str, target: Path, env: dict) -> dict:
    argv = [out / "Harness", "run", out / "Kernels.metallib", max_count, order, mode, target]
    result = sh(argv, env=env, timeout=3600)
    log = target.with_suffix(".log")
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        return {"crashed": result.returncode, "log": (result.stdout + result.stderr)[-4000:]}
    data = json.loads(target.read_text())
    data["powerSource"] = power_source()
    data["stderrLines"] = len([line for line in result.stderr.splitlines()
                               if "Validation Enabled" not in line])
    target.write_text(json.dumps(data) + "\n")
    return data


def correctness(out: Path) -> None:
    """Images are judged under API validation; the run with shader validation must report no
    error. Shader validation drops kernel-encoded ICB commands without an error, so its image
    mismatches are recorded as a finding, not as a gate failure."""
    folder = out / "correctness"
    folder.mkdir(parents=True, exist_ok=True)
    failed = False
    for max_count in MAX_COUNTS:
        for shader in (False, True):
            name = "shader" if shader else "api"
            data = harness(out, max_count, 0, "correctness", folder / f"m{max_count}-{name}.json",
                           validation_env(shader))
            if "crashed" in data:
                failed = True
                print(f"maxCount={max_count} {name}: process exited {data['crashed']}\n"
                      f"{data['log']}")
                continue
            errors = [r for r in data["results"] if r["gpuError"]]
            wrong = [r for r in data["results"] if r["mismatches"]]
            failed |= bool(errors) or data["stderrLines"] > 0 or (not shader and bool(wrong))
            print(f"maxCount={max_count} {name}: {len(data['results'])} cases, "
                  f"{len(errors)} GPU errors, {data['stderrLines']} stderr lines, "
                  f"{len(wrong)} image mismatches{' (finding)' if shader and wrong else ''}")
            for r in errors + (wrong if not shader else []):
                print("  ", {k: r[k] for k in ("variant", "fill", "arm", "mismatches",
                                                "firstBad", "gpuError")})
            if shader and wrong:
                print("   dropped:", sorted({(r["variant"], r["arm"]) for r in wrong}))
    print("correctness gate:", "FAILED" if failed else "passed")


def timing(out: Path) -> None:
    folder = out / "timing"
    folder.mkdir(parents=True, exist_ok=True)
    for process in range(PROCESSES):
        for max_count in MAX_COUNTS:
            target = folder / f"m{max_count}-p{process:02d}.json"
            if target.exists():
                continue
            for attempt in range(4):
                data = harness(out, max_count, process, "timing", target, timing_env())
                if "crashed" in data:
                    sys.exit(f"maxCount={max_count} process {process} exited "
                             f"{data['crashed']}\n{data['log']}")
                if data["thermalStart"] == "nominal" and data["thermalEnd"] == "nominal":
                    break
                discard = target.with_suffix(f".discarded{attempt}.json")
                target.rename(discard)
                print(f"maxCount={max_count} process {process}: thermal {data['thermalStart']}->"
                      f"{data['thermalEnd']}, discarded; cooling down", flush=True)
                time.sleep(120)
            else:
                sys.exit(f"maxCount={max_count} process {process}: thermal state never nominal")
            print(f"maxCount={max_count} process {process} done", flush=True)


def bootstrap(deltas: list[float], rng: random.Random) -> tuple[float, float]:
    stats = sorted(
        statistics.median(rng.choices(deltas, k=len(deltas))) for _ in range(RESAMPLES))
    return stats[int(0.025 * RESAMPLES)], stats[int(0.975 * RESAMPLES) - 1]


def compare(rows: list[dict], numerator: str, denominator: str, rng: random.Random) -> dict:
    """Paired delta numerator - denominator; material against the faster arm's median."""
    deltas = [p[numerator] - p[denominator] for p in rows]
    base = min(statistics.median(p[numerator] for p in rows),
               statistics.median(p[denominator] for p in rows))
    delta = statistics.median(deltas)
    low, high = bootstrap(deltas, rng)
    material = (low > 0 or high < 0) and abs(delta) > BUDGET * base
    return {"delta": delta, "low": low, "high": high, "relative": delta / base if base else None,
            "material": material}


def analyze(out: Path) -> None:
    rng = random.Random(20261012)
    runs = [json.loads(p.read_text()) for p in sorted((out / "timing").glob("m*-p??.json"))]
    gpu, cpu, calls, memory, bad = {}, {}, {}, {}, []
    for run in runs:
        memory[run["maxCount"]] = run["memory"]
        for r in run["results"]:
            key = (run["maxCount"], r["variant"], r["fill"])
            gpu.setdefault(key, {}).setdefault(run["order"], {})[r["arm"]] = r["gpuMedianNs"]
            cpu.setdefault(key, {}).setdefault(run["order"], {})[r["arm"]] = r["cpuMedianNs"]
            calls.setdefault(key, {})[r["arm"]] = r["calls"]
            if r["mismatches"] or r["gpuError"]:
                bad.append((key, r["arm"], run["order"]))
    summary = {"rows": [], "memory": memory, "timedMismatches": bad}
    for key in sorted(gpu):
        g, c = list(gpu[key].values()), list(cpu[key].values())
        entry = {"maxCount": key[0], "variant": key[1], "fill": key[2], "processes": len(g),
                 "calls": calls[key],
                 "gpuMedianNs": {a: statistics.median(p[a] for p in g) for a in g[0]},
                 "cpuMedianNs": {a: statistics.median(p[a] for p in c) for a in c[0]}}
        for left, right in (("E", "L"), ("E", "N"), ("L", "N"), ("N0", "N")):
            entry[f"gpu{left}vs{right}"] = compare(g, left, right, rng)
            entry[f"cpu{left}vs{right}"] = compare(c, left, right, rng)
        summary["rows"].append(entry)
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    def rel(x):
        return f"{x['relative']:+.1%}" if x["relative"] is not None else "n/a"

    print("median of process medians (us); E-L and N0-N relative to the faster arm")
    for e in summary["rows"]:
        g, c = e["gpuMedianNs"], e["cpuMedianNs"]
        print(f"max={e['maxCount']:>5} {e['variant']:>7} {e['fill']:>7} | GPU "
              + " ".join(f"{a}={g[a] / 1000:.1f}" for a in ("E", "L", "N", "N0"))
              + f" E-L {rel(e['gpuEvsL'])}{'*' if e['gpuEvsL']['material'] else ''}"
              + f" N0-N {rel(e['gpuN0vsN'])}{'*' if e['gpuN0vsN']['material'] else ''} | CPU "
              + " ".join(f"{a}={c[a] / 1000:.1f}" for a in ("E", "L", "N", "N0"))
              + f" E-L {rel(e['cpuEvsL'])}{'*' if e['cpuEvsL']['material'] else ''}"
              + f" | calls {e['calls']}")
    print("memory:", json.dumps(memory))
    print("timed mismatches:", bad)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in {"build", "correctness", "timing", "analyze"}:
        sys.exit(__doc__)
    {"build": build, "correctness": correctness, "timing": timing,
     "analyze": analyze}[sys.argv[1]](Path(sys.argv[2]).resolve())
