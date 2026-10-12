"""Build and run spike 2; throwaway spike source.

    python3 Tools/Spikes/TextureIndexing/run.py build OUT
    python3 Tools/Spikes/TextureIndexing/run.py correctness OUT
    python3 Tools/Spikes/TextureIndexing/run.py timing OUT
    python3 Tools/Spikes/TextureIndexing/run.py analyze OUT

OUT is an evidence directory outside the repository. The protocol is fixed in
docs/research/2026-10-12-spike-2-texture-indexing.md.
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
COUNTS = [1, 16, 128, 1024, 16384, 100000]
FIXED_MAX = 128
PROCESSES = 20
RESAMPLES = 10000
BUDGET = 0.05
HOT_COUNT = 1024


def sh(argv, **kw):
    return subprocess.run(list(map(str, argv)), text=True, capture_output=True, **kw)


def build(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    steps = [
        ["xcrun", "-sdk", "macosx", "metal", "-std=metal4.0", f"-DFIXED_MAX={FIXED_MAX}",
         SOURCE / "Kernels.metal", "-o", out / "Kernels.metallib"],
        ["xcrun", "clang++", "-std=c++23", "-O2", "-fblocks", "-Wall", "-Wextra", "-I",
         ROOT / "ThirdParty/metal-cpp", SOURCE / "Harness.cpp", "-framework", "Foundation",
         "-framework", "Metal", "-o", out / "Harness"],
    ]
    for argv in steps:
        result = sh(argv)
        if result.returncode:
            sys.exit(result.stdout + result.stderr)
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


def validation_env() -> dict:
    env = os.environ.copy()
    env.update({"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"})
    return env


def timing_env() -> dict:
    env = os.environ.copy()
    for key in [k for k in env if k.startswith(("MTL_", "METAL_"))]:
        del env[key]
    return env


def power_source() -> str:
    first = sh(["pmset", "-g", "batt"]).stdout.splitlines()[:1]
    return first[0].split("'")[1] if first and "'" in first[0] else "unknown"


def harness(out: Path, count: int, order: int, mode: str, target: Path, env: dict) -> dict:
    argv = [out / "Harness", "run", out / "Kernels.metallib", count, order, mode, FIXED_MAX, target]
    result = sh(argv, env=env, timeout=1800)
    if result.returncode:
        sys.exit(f"{' '.join(map(str, argv))}\n{result.stdout}{result.stderr}")
    data = json.loads(target.read_text())
    data["powerSource"] = power_source()
    target.write_text(json.dumps(data) + "\n")
    return data


def correctness(out: Path) -> None:
    folder = out / "correctness"
    folder.mkdir(parents=True, exist_ok=True)
    failed = False
    for count in COUNTS:
        data = harness(out, count, 0, "correctness", folder / f"n{count}.json", validation_env())
        bad = [r for r in data["results"] if r["mismatches"] or r["gpuError"]]
        failed |= bool(bad) or data["poolNonContiguous"] != 0
        print(f"N={count}: {len(data['results'])} cases, {len(bad)} failing, "
              f"pool non-contiguous {data['poolNonContiguous']}")
        for r in bad:
            print("  ", r)
    print("correctness gate:", "FAILED" if failed else "passed")


def timing(out: Path) -> None:
    folder = out / "timing"
    folder.mkdir(parents=True, exist_ok=True)
    for process in range(PROCESSES):
        for count in COUNTS:
            target = folder / f"n{count}-p{process:02d}.json"
            if target.exists():
                continue
            for attempt in range(4):
                data = harness(out, count, process, "timing", target, timing_env())
                if data["thermalStart"] == "nominal" and data["thermalEnd"] == "nominal":
                    break
                discard = target.with_suffix(f".discarded{attempt}.json")
                target.rename(discard)
                print(f"N={count} process {process}: thermal {data['thermalStart']}->"
                      f"{data['thermalEnd']}, discarded; cooling down", flush=True)
                time.sleep(120)
            else:
                sys.exit(f"N={count} process {process}: thermal state never nominal")
            print(f"N={count} process {process} done", flush=True)


def bootstrap(deltas: list[float], rng: random.Random) -> tuple[float, float]:
    stats = sorted(
        statistics.median(rng.choices(deltas, k=len(deltas))) for _ in range(RESAMPLES))
    return stats[int(0.025 * RESAMPLES)], stats[int(0.975 * RESAMPLES) - 1]


def compare(rows: list[dict], numerator: str, denominator: str, rng: random.Random) -> dict:
    deltas = [p[numerator] - p[denominator] for p in rows]
    base = statistics.median(p[denominator] for p in rows)
    delta = statistics.median(deltas)
    low, high = bootstrap(deltas, rng)
    material = (low > 0 or high < 0) and abs(delta) > BUDGET * base
    return {"delta": delta, "low": low, "high": high, "relative": delta / base,
            "relLow": low / base, "relHigh": high / base, "material": material}


def analyze(out: Path) -> None:
    rng = random.Random(20261012)
    runs = [json.loads(p.read_text()) for p in sorted((out / "timing").glob("n*-p??.json"))]
    gpu, publish = {}, {}
    for run in runs:
        for r in run["results"]:
            key = (run["count"], r["pattern"], r["access"])
            gpu.setdefault(key, {}).setdefault(run["order"], {})[r["arm"]] = r["medianNs"]
        publish.setdefault(run["count"], []).append(run["publishNsPerTexture"])
    summary = {"gpu": [], "publish": []}
    for (count, pattern, access), by_process in sorted(gpu.items()):
        rows = list(by_process.values())
        entry = {"count": count, "pattern": pattern, "access": access, "processes": len(rows),
                 "medianNs": {arm: statistics.median(p[arm] for p in rows) for arm in rows[0]},
                 "TvsP": compare(rows, "T", "P", rng)}
        if "F" in rows[0]:
            entry["TvsF"] = compare(rows, "T", "F", rng)
            entry["PvsF"] = compare(rows, "P", "F", rng)
        if "TS" in rows[0]:
            entry["TSvsT"] = compare(rows, "TS", "T", rng)
        summary["gpu"].append(entry)
    for count, rows in sorted(publish.items()):
        entry = {"count": count, "processes": len(rows),
                 "medianNs": {arm: statistics.median(p[arm] for p in rows) for arm in rows[0]},
                 "TvsP": compare(rows, "T", "P", rng)}
        summary["publish"].append(entry)
    hot = [e for e in summary["gpu"] if e["count"] >= HOT_COUNT]
    pool_wins = [e for e in hot if e["TvsP"]["material"] and e["TvsP"]["delta"] > 0]
    summary["decision"] = {
        "materialPoolWins": [(e["count"], e["pattern"], e["access"]) for e in pool_wins],
        "rule": "table stays unless P passes correctness and T's tax over P is material at "
                "N >= 1024 (95% CI excludes zero, effect > 5% of P)",
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("GPU kernel, median of process medians (us); T over P, 95% CI")
    for e in summary["gpu"]:
        m, c = e["medianNs"], e["TvsP"]
        arms = " ".join(f"{a}={m[a] / 1000:.1f}" for a in ("F", "T", "P", "TS") if a in m)
        print(f"N={e['count']:>6} {e['pattern']:>9} {e['access']:>6}: {arms} | "
              f"T-P {c['relative']:+.2%} [{c['relLow']:+.2%}, {c['relHigh']:+.2%}]"
              f"{' MATERIAL' if c['material'] else ''}")
    print("CPU publish, ns per texture")
    for e in summary["publish"]:
        m, c = e["medianNs"], e["TvsP"]
        print(f"N={e['count']:>6}: " + " ".join(f"{a}={v:.1f}" for a, v in sorted(m.items()))
              + f" | T-P {c['relative']:+.1%} [{c['relLow']:+.1%}, {c['relHigh']:+.1%}]")
    print("pool material wins at hot configurations:", summary["decision"]["materialPoolWins"])


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in {"build", "correctness", "timing", "analyze"}:
        sys.exit(__doc__)
    {"build": build, "correctness": correctness, "timing": timing,
     "analyze": analyze}[sys.argv[1]](Path(sys.argv[2]).resolve())
