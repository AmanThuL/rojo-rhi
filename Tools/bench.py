#!/usr/bin/env python3
"""Drive the Metal 4 overhead benchmark round: build, correctness gate, timed processes, analysis.

The harness is the `RojoRHIBench` executable; this driver builds it, runs it one process at a time
and keeps every result under `<root>/<topic>/<date>/` with checksums. An interrupted run resumes:
a result that exists is never run again.

    python3 Tools/bench.py build
    python3 Tools/bench.py correctness
    python3 Tools/bench.py timing
    python3 Tools/bench.py analyze
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

if not __package__:  # run as a script: make `Tools` importable from the repository root
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from Tools import bench_analysis as analysis  # noqa: E402
from Tools import gpu_evidence  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = "RojoRHIBench"
D3D12_EXECUTABLE = "RojoRHIBench.exe"
MODES = ("release", "debug")

# Statistics, recorded per run: a process above the nominal thermal state is discarded and rerun
# after a rest, at most four attempts in all.
THERMAL_NOMINAL = "nominal"
MAX_ATTEMPTS = 4
REST_SECONDS = 120

# Metrics 1, standard error: the validation layer's banner is the only line a run may print.
STDERR_ALLOWLIST = ("Validation Enabled",)

# Statistics, thermal state on Windows (D3D12): there is no thermalState, so a vendor CLI is read
# before and after each timed process. A process is discarded when the temperature rose by more than
# TEMPERATURE_RISE_LIMIT degrees or the end clock fell below CLOCK_FLOOR of the round's median start
# clock; without the CLI the record says so and the driver rests between processes.
GPU_QUERY = ["nvidia-smi", "--query-gpu=temperature.gpu,clocks.sm", "--format=csv,noheader,nounits"]
TEMPERATURE_RISE_LIMIT = 10
CLOCK_FLOOR = 0.95
NOT_MONITORED = "not monitored"
UNMONITORED_REST_SECONDS = 30
WARP_REFUSAL = "timing on WARP measures a software rasterizer, not a cost (ADR 0014)"

# Build, d3d12: the debug features each mode configures (the layer in Debug, nothing in Release).
D3D12_DEBUG = {"release": "mode", "debug": "layer"}

# Statistics, power: timed processes run on mains power only.
MAINS_POWER = "AC Power"

# The driver's own record of the build: backend, the harness's environment, power and the removed
# variables. `environment.json` belongs to Tools/gpu_evidence.py, which rewrites it.
RECORD_FILE = "bench-environment.json"

KEPT_RESULT = re.compile(r"^(?P<id>.+)-p(?P<process>\d\d)\.json$")


class BenchError(Exception):
    """A condition that stops a command, with the message printed on standard error."""


@dataclass
class Hooks:
    """What the driver reaches the outside world through, replaceable in tests."""

    run: Callable[..., subprocess.CompletedProcess]
    sleep: Callable[[float], None]
    clock: Callable[[], float]
    power: Callable[[], str]
    environment: Callable[[], dict]
    base_env: Mapping[str, str]


def run_process(argv, cwd=None, env=None) -> subprocess.CompletedProcess:
    """Run a command to completion, capturing both streams as text."""
    return subprocess.run(argv, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


class SystemPowerStatus(ctypes.Structure):
    """The Win32 SYSTEM_POWER_STATUS record `GetSystemPowerStatus` fills."""

    _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte),
                ("BatteryLifePercent", ctypes.c_ubyte), ("SystemStatusFlag", ctypes.c_ubyte),
                ("BatteryLifeTime", ctypes.c_uint32), ("BatteryFullLifeTime", ctypes.c_uint32)]


def probe_windows_power() -> str:
    """Return the power source `GetSystemPowerStatus` reports, as `probe_power` does."""
    status = SystemPowerStatus()
    try:
        kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        read = kernel32.GetSystemPowerStatus(ctypes.byref(status))
    except (AttributeError, OSError):
        return "unknown"
    if not read:
        return "unknown"
    return {1: MAINS_POWER, 0: "Battery Power"}.get(status.ACLineStatus, "unknown")


def probe_power() -> str:
    """Return the power source the machine reports: "AC Power", "Battery Power" or "unknown"."""
    if sys.platform == "win32":
        return probe_windows_power()
    try:
        result = run_process(["pmset", "-g", "batt"])
    except OSError:
        return "unknown"
    for source in (MAINS_POWER, "Battery Power"):
        if source in result.stdout:
            return source
    return "unknown"


def default_hooks() -> Hooks:
    """Return the hooks that touch the real machine."""
    return Hooks(run=run_process, sleep=time.sleep, clock=time.monotonic, power=probe_power,
                 environment=gpu_evidence.environment, base_env=dict(os.environ))


# Metrics 3: the allocation counter reads nothing under libmalloc debugging, so every Malloc*
# variable is removed beside the Metal ones.
STRIPPED_PREFIXES = ("MTL_", "METAL_", "Malloc")


def strip_metal(env: Mapping[str, str]) -> dict[str, str]:
    """Return the environment without any MTL_*, METAL_* or Malloc* variable."""
    return {key: value for key, value in env.items() if not key.startswith(STRIPPED_PREFIXES)}


def stripped_names(env: Mapping[str, str]) -> list[str]:
    """Return the sorted names of the variables `strip_metal` removes."""
    return sorted(key for key in env if key.startswith(STRIPPED_PREFIXES))


def read_json(path: Path):
    """Read a JSON file."""
    return json.loads(path.read_text())


def write_json(path: Path, value) -> None:
    """Write a JSON file with a trailing newline."""
    path.write_text(json.dumps(value, indent=2) + "\n")


def stray_lines(log: str) -> list[str]:
    """Return the non-empty lines of a standard-error log that the allowlist does not cover."""
    return [line for line in log.splitlines()
            if line.strip() and not any(allowed in line for allowed in STDERR_ALLOWLIST)]


class Driver:
    """The commands of one evidence directory."""

    def __init__(self, out: Path, hooks: Hooks, repo: Path = ROOT) -> None:
        self.out = out
        self.hooks = hooks
        self.repo = repo
        self.rest_pending = False
        out.mkdir(parents=True, exist_ok=True)

    # Helpers.

    def executable(self, mode: str, backend: str | None = None) -> Path:
        """Return the copied harness binary of a build mode, named for the recorded backend."""
        name = D3D12_EXECUTABLE if (backend or self.backend()) == "d3d12" else EXECUTABLE
        return self.out / "bin" / mode / name

    def configurations(self) -> list[dict]:
        """Return the configurations `list` printed at build time."""
        path = self.out / "configurations.json"
        if not path.exists():
            raise BenchError(f"{path} is missing: run the build command first")
        return read_json(path)

    def backend(self) -> str:
        """Return the backend the binaries were built for."""
        path = self.out / RECORD_FILE
        if not path.exists():
            raise BenchError(f"{path} is missing: run the build command first")
        return read_json(path)["backend"]

    def adapter(self) -> str | None:
        """Return the adapter a d3d12 build selects, None for the other backends."""
        return read_json(self.out / RECORD_FILE).get("adapter")

    def selected(self, only: list[str] | None) -> list[dict]:
        """Return the configurations of the requested workloads, in list order."""
        configs = self.configurations()
        return [c for c in configs if only is None or c["workload"] in only]

    def record_stripped(self) -> None:
        """Add the variables removed from child environments to the driver's record."""
        path = self.out / RECORD_FILE
        environment = read_json(path)
        names = sorted(set(environment.get("strippedEnvironment", []))
                       | set(stripped_names(self.hooks.base_env)))
        if names != environment.get("strippedEnvironment"):
            environment["strippedEnvironment"] = names
            write_json(path, environment)

    def check_power(self) -> str:
        """Return the power source, refusing anything but mains power."""
        power = self.hooks.power()
        if power != MAINS_POWER:
            raise BenchError(f"timed processes need mains power, the machine reports: {power} "
                             "(battery or unknown)")
        return power

    def say(self, message: str) -> None:
        """Print a progress line."""
        print(message, flush=True)

    # Build.

    def xmake(self, *arguments: str) -> None:
        """Run xmake in the repository, failing with its message."""
        result = self.hooks.run(["xmake", *arguments], cwd=self.repo)
        if result.returncode:
            text = (result.stderr or result.stdout).strip()
            raise BenchError(f"xmake {' '.join(arguments)} failed:\n{text}")

    def build(self, backend: str, adapter: str | None = None) -> None:
        """Build both modes, copy the binaries and record the configurations and environment."""
        if adapter is not None and backend != "d3d12":
            raise BenchError("--adapter applies to the d3d12 backend only")
        adapter = adapter or ("hardware" if backend == "d3d12" else None)
        for mode in MODES:
            if backend == "d3d12":
                debug = D3D12_DEBUG[mode]
                self.xmake("f", "-m", mode, "-y", f"--rojorhi_backend={backend}",
                           f"--rojorhi_d3d12_adapter={adapter}", f"--rojorhi_d3d12_debug={debug}")
            else:
                self.xmake("f", "-m", mode, "-y", f"--rojorhi_backend={backend}")
            self.xmake("build", EXECUTABLE)
            target = self.out / "bin" / mode
            target.mkdir(parents=True, exist_ok=True)
            if backend == "d3d12":
                # The executable needs the Agility runtime, the shader libraries and WARP beside it.
                source = self.repo / "build" / "windows" / "x64" / mode / "rojorhi-bench"
                shutil.copytree(source, target, dirs_exist_ok=True)
            else:
                source = self.repo / "build" / "macosx" / "arm64" / mode / "rojorhi-bench"
                shutil.copy2(source / EXECUTABLE, target / EXECUTABLE)
                shutil.copytree(source / "Shaders", target / "Shaders", dirs_exist_ok=True)
        self.xmake("f", "-m", "debug", "-y", "--rojorhi_backend=null")
        release = str(self.executable("release", backend))
        listed = self.hooks.run([release, "list"], cwd=self.repo)
        if listed.returncode:
            raise BenchError(f"{release} list failed:\n{listed.stderr.strip()}")
        write_json(self.out / "configurations.json",
                   [json.loads(line) for line in listed.stdout.splitlines() if line.strip()])
        scratch = self.out / "bench-harness-environment.partial"
        described = self.hooks.run([release, "environment", str(scratch)], cwd=self.repo)
        if described.returncode:
            raise BenchError(f"{release} environment failed:\n{described.stderr.strip()}")
        bench_environment = read_json(scratch)
        scratch.unlink()
        write_json(self.out / RECORD_FILE, {
            **self.hooks.environment(), "bench": bench_environment, "backend": backend,
            **({"adapter": adapter, "debug": D3D12_DEBUG} if backend == "d3d12" else {}),
            "power": self.hooks.power(),
            "strippedEnvironment": stripped_names(self.hooks.base_env),
        })

    # Correctness gate.

    def run_correctness(self, config: dict, mode: str, arms: str) -> None:
        """Run one configuration in one build unless its result exists."""
        directory = self.out / "correctness"
        name = f"{config['id']}.{mode}"
        result_path = directory / f"{name}.json"
        if result_path.exists() and set(arms.split(",")) <= set(read_json(result_path)["arms"]):
            return
        env = strip_metal(self.hooks.base_env)
        arguments = [str(self.executable(mode)), "run", config["id"], "--mode", "correctness",
                     "--order", "RN", "--arms", arms, "--dump", str(directory / "dumps" / mode)]
        if mode == "debug":
            if self.backend() != "d3d12":  # the D3D12 debug layer is built in
                env["MTL_DEBUG_LAYER"] = "1"
            if "R" in arms.split(","):
                arguments.append("--validation")
        partial = directory / f"{name}.partial"
        result = self.hooks.run([*arguments, "--out", str(partial)], cwd=self.repo, env=env)
        (directory / f"{name}.log").write_text(result.stderr or "")
        if result.returncode == 0 and partial.exists():
            partial.replace(result_path)

    def evaluate_run(self, config: dict, mode: str) -> tuple[list[str], bool]:
        """Return the failures of one kept correctness run, read back from disk, and whether R and
        N both ran and had their dumps compared."""
        directory = self.out / "correctness"
        name = f"{config['id']}.{mode}"
        label = f"{config['id']} {mode}"
        path = directory / f"{name}.json"
        if not path.exists():
            return [f"{label}: no result, see {directory / (name + '.log')}"], False
        failures = []
        result = read_json(path)
        for arm_name, arm in result["arms"].items():
            if arm["status"] != "ok":
                failures.append(f"{label}: arm {arm_name} failed: {arm.get('error', '')}")
        for line in stray_lines((directory / f"{name}.log").read_text()):
            failures.append(f"{label}: standard error: {line}")
        arms = result["arms"]
        compared = "R" in arms and "N" in arms
        if compared:
            if result.get("outputsEqual") is not True:
                failures.append(f"{label}: outputsEqual is {result.get('outputsEqual')}")
            dumps = directory / "dumps" / mode
            for output in arms["N"]["outputs"]:
                files = [dumps / f"{config['id']}.{arm}.{output['name']}.bin" for arm in ("R", "N")]
                missing = [str(f) for f in files if not f.exists()]
                if missing:
                    failures.append(f"{label}: output {output['name']}: missing dump {missing[0]}")
                elif files[0].read_bytes() != files[1].read_bytes():
                    failures.append(f"{label}: output {output['name']}: R and N differ")
        return failures, compared

    def native_count_failures(self, workload: str, configs: list[dict]) -> list[str]:
        """Return a failure when N's calls per repetition differ from the registered column."""
        registered = analysis.native_calls_per_repetition(self.backend()).get(workload)
        if registered is None:
            return []
        k_hi, k_lo = analysis.K_PAIRS.get(workload, analysis.DEFAULT_K_PAIR)
        by_k = {c["params"].get("K"): c["id"] for c in configs if c["workload"] == workload}
        counts = []
        for k in (k_hi, k_lo):
            if k not in by_k:
                return [f"{workload}: no configuration at K {k} to count native calls with"]
            path = self.out / "correctness" / f"{by_k[k]}.release.json"
            if not path.exists():
                return []  # the missing run is already a failure of that configuration
            arm = read_json(path)["arms"].get("N")
            calls = arm.get("nativeCalls") if arm else None
            if not calls or calls[0] is None:
                return [f"{by_k[k]}: no native call count of N in the first measured frame"]
            counts.append(calls[0])
        measured = analysis.per_operation_count(counts[0], counts[1], k_hi, k_lo)["value"]
        if abs(measured - registered) > 1e-9:
            return [f"{workload}: N makes {measured} native calls per repetition, "
                    f"the registered minimum is {registered}"]
        return []

    def correctness(self, arms: str = "R,N", only: list[str] | None = None) -> None:
        """Run the gate for the selected workloads and write correctness/summary.json."""
        if self.backend() == "null" and "R" in arms.split(","):
            raise BenchError("R renders nothing on a Null build: pass --arms N")
        self.record_stripped()
        configs = self.configurations()
        selected = self.selected(only)
        directory = self.out / "correctness"
        (directory / "dumps").mkdir(parents=True, exist_ok=True)
        for config in selected:
            for mode in MODES:
                self.run_correctness(config, mode, arms)
        summary_path = directory / "summary.json"
        summary = read_json(summary_path) if summary_path.exists() else {}
        for workload in dict.fromkeys(c["workload"] for c in selected):
            failures: list[str] = []
            compared = True
            for config in (c for c in selected if c["workload"] == workload):
                for mode in MODES:
                    found, was_compared = self.evaluate_run(config, mode)
                    failures += found
                    compared = compared and was_compared
            failures += self.native_count_failures(workload, configs)
            # Only a run in which R and N both ran and were compared can pass the gate; an
            # N-only or R-only run is a smoke that timing does not accept.
            passed = compared and not failures
            summary[workload] = {"passed": passed, "failures": failures,
                                 "gate": "compared" if compared else "smoke"}
            outcome = "passed" if passed else ("FAILED" if failures else "smoke only, not a pass")
            self.say(f"correctness {workload}: {outcome}")
            for failure in failures:
                self.say(f"  {failure}")
        write_json(summary_path, summary)

    # Timing.

    def timing(self, only: list[str] | None = None) -> None:
        """Run the timed processes of every workload whose correctness passed."""
        if self.backend() == "null":
            raise BenchError("timing is refused on a Null build: it measures nothing")
        if self.backend() == "d3d12" and self.adapter() == "warp":
            raise BenchError(WARP_REFUSAL)
        self.record_stripped()
        gate_path = self.out / "correctness" / "summary.json"
        gate = read_json(gate_path) if gate_path.exists() else {}
        configs = self.selected(only)
        (self.out / "timing").mkdir(exist_ok=True)
        for workload in dict.fromkeys(c["workload"] for c in configs):
            if not gate.get(workload, {}).get("passed"):
                self.say(f"timing {workload}: skipped, correctness has not passed")
                continue
            members = [c for c in configs if c["workload"] == workload]
            for process in range(analysis.PROCESSES):
                for config in members:
                    # Unmonitored: rest before the next process, never after the last.
                    if self.rest_pending:
                        self.rest_pending = False
                        self.hooks.sleep(UNMONITORED_REST_SECONDS)
                    self.timed_process(config, process)

    def timed_process(self, config: dict, process: int) -> None:
        """Run one process of a configuration, with discards and reruns, unless it is kept."""
        timing = self.out / "timing"
        stem = f"{config['id']}-p{process:02d}"
        final = timing / f"{stem}.json"
        notrun = timing / f"{config['id']}.notrun.json"
        if final.exists() or notrun.exists():
            return
        attempt = 1 + len(list(timing.glob(f"{stem}.discarded*.json")))
        if 1 < attempt <= MAX_ATTEMPTS:
            self.hooks.sleep(REST_SECONDS)  # an interrupted rest is taken again
        while True:
            if attempt > MAX_ATTEMPTS:
                write_json(notrun, {
                    "id": config["id"], "process": process, "attempts": MAX_ATTEMPTS,
                    "reason": f"thermal state above nominal in {MAX_ATTEMPTS} attempts "
                              f"at process {process}"})
                self.say(f"{config['id']}: not run, thermal state")
                return
            power = self.check_power()
            outcome = self.attempt(config, process, attempt, power)
            if outcome == THERMAL_NOMINAL:
                return
            if attempt == MAX_ATTEMPTS:
                write_json(notrun, {"id": config["id"], "process": process, "attempts": attempt,
                                    "reason": f"thermal state {outcome} after {attempt} attempts "
                                              f"at process {process}"})
                self.say(f"{config['id']}: not run, thermal state {outcome}")
                return
            self.hooks.sleep(REST_SECONDS)
            attempt += 1

    def attempt(self, config: dict, process: int, attempt: int, power: str) -> str:
        """Run one attempt; keep or discard its result. Return "nominal" or the hot state."""
        timing = self.out / "timing"
        stem = f"{config['id']}-p{process:02d}"
        order = "RN" if process % 2 == 0 else "NR"
        partial = timing / f"{stem}.partial"
        arguments = [str(self.executable("release")), "run", config["id"], "--mode", "timing",
                     "--order", order, "--arms", ",".join(config["arms"]), "--out", str(partial)]
        d3d12 = self.backend() == "d3d12"
        before = self.read_gpu() if d3d12 else None
        started = self.hooks.clock()
        result = self.hooks.run(arguments, cwd=self.repo, env=strip_metal(self.hooks.base_env))
        after = self.read_gpu() if d3d12 else None
        log = timing / f"{stem}.log"
        log.write_text(result.stderr or "")
        if result.returncode or not partial.exists():
            raise BenchError(f"{config['id']} process {process}: harness exit "
                             f"{result.returncode}, see {log}")
        # A timed process prints nothing: a line on standard error, such as RojoRHI's warning that a counter heap was
        # refused and a scope went untimed, means the process did different work from the gate's (issue #64).
        stray = stray_lines(result.stderr or "")
        if stray:
            partial.rename(timing / f"{stem}.failed.json")
            raise BenchError(f"{config['id']} process {process}: standard error: {stray[0]}, see {log}")
        data = read_json(partial)
        failed = [name for name, arm in data["arms"].items() if arm["status"] != "ok"]
        if failed:
            partial.rename(timing / f"{stem}.failed.json")
            raise BenchError(f"{config['id']} process {process}: arm {failed[0]} failed, "
                             f"see {log}")
        data["power"] = power
        data["attempt"] = attempt
        if d3d12:
            hot = self.judge_gpu(data, before, after)
        else:
            hot = next((state for state in (data["thermalStart"], data["thermalEnd"])
                        if state != THERMAL_NOMINAL), THERMAL_NOMINAL)
        write_json(partial, data)
        if hot == THERMAL_NOMINAL:
            partial.rename(timing / f"{stem}.json")
        else:
            partial.rename(timing / f"{stem}.discarded{attempt}.json")
            log.rename(timing / f"{stem}.discarded{attempt}.log")
        self.say(f"{stem} attempt {attempt}: {hot}, {self.hooks.clock() - started:.0f}s")
        if data.get("thermal") == NOT_MONITORED:
            self.rest_pending = True
        return hot

    def read_gpu(self) -> tuple[float, float] | None:
        """Return the GPU's (temperature in degrees, shader clock in MHz) from the vendor CLI, or
        None when it is missing, fails or prints something else."""
        # The timing machine has one NVIDIA GPU (the round records it): its reading is line one.
        try:
            result = self.hooks.run(GPU_QUERY, cwd=self.repo)
        except OSError:
            return None
        lines = (result.stdout or "").strip().splitlines()
        if result.returncode or not lines:
            return None
        try:
            temperature, clock = (float(field) for field in lines[0].split(","))
        except ValueError:
            return None
        return temperature, clock

    def round_start_clocks(self) -> list[float]:
        """Return the start clock of every kept, monitored process of the round."""
        clocks = []
        for path in (self.out / "timing").glob("*.json"):
            if ".discarded" in path.name:
                continue
            thermal = read_json(path).get("thermal")
            if isinstance(thermal, dict):
                clocks.append(thermal["startClockMHz"])
        return clocks

    def judge_gpu(self, data: dict, before: tuple[float, float] | None,
                  after: tuple[float, float] | None) -> str:
        """Record the GPU readings in the result and return "nominal" or why it is discarded."""
        if before is None or after is None:
            data["thermal"] = NOT_MONITORED
            return THERMAL_NOMINAL
        data["thermal"] = {"startC": before[0], "endC": after[0],
                           "startClockMHz": before[1], "endClockMHz": after[1]}
        if after[0] - before[0] > TEMPERATURE_RISE_LIMIT:
            return "temperature rise"
        clocks = self.round_start_clocks() + [before[1]]
        if after[1] < CLOCK_FLOOR * analysis.median(clocks):
            return "clock drop"
        return THERMAL_NOMINAL

    # Analysis.

    def analyze(self) -> None:
        """Write summary.json and analysis.txt from the kept results and print the tables."""
        configs = self.configurations()
        processes: dict[str, dict[int, dict]] = {}
        notruns: dict[str, str] = {}
        timing = self.out / "timing"
        if timing.exists():
            for path in sorted(timing.iterdir()):
                kept = KEPT_RESULT.match(path.name)
                if kept:
                    processes.setdefault(kept["id"], {})[int(kept["process"])] = read_json(path)
                elif path.name.endswith(".notrun.json"):
                    notruns[path.name.removesuffix(".notrun.json")] = read_json(path)["reason"]
        gate_path = self.out / "correctness" / "summary.json"
        gate = read_json(gate_path) if gate_path.exists() else {}
        result = analysis.summary(configs, processes, notruns, gate, self.backend())
        write_json(self.out / "summary.json", result)
        report = analysis.format_report(result)
        (self.out / "analysis.txt").write_text(report)
        print(report, end="")


def parse_arguments(argv: list[str] | None) -> argparse.Namespace:
    """Parse the command line."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=gpu_evidence.DEFAULT_EVIDENCE_ROOT,
                        help="evidence root; defaults to rojo-rhi-evidence by the main checkout")
    parser.add_argument("--topic", default="t2", help="directory under the root")
    parser.add_argument("--date", default=datetime.date.today().isoformat(), help="YYYY-MM-DD")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="build and copy the Release and Debug binaries")
    build.add_argument("--backend", choices=["metal4", "null", "d3d12"], default="metal4")
    build.add_argument("--adapter", choices=["hardware", "warp"],
                       help="d3d12 only: the adapter the build selects (default hardware)")
    correctness = commands.add_parser("correctness", help="run the correctness gate")
    correctness.add_argument("--arms", default="R,N", help="arms to run, such as N on a Null build")
    for command in (correctness, commands.add_parser("timing", help="run the timed processes")):
        command.add_argument("--only", help="comma-separated workloads, such as C1,G2")
    commands.add_parser("analyze", help="write summary.json and analysis.txt")
    args = parser.parse_args(argv)
    if args.command == "build" and args.adapter and args.backend != "d3d12":
        parser.error("--adapter applies to the d3d12 backend only")
    return args


def main(argv: list[str] | None = None, hooks: Hooks | None = None) -> int:
    """Run one command; return 0, or 1 with the reason on standard error."""
    args = parse_arguments(argv)
    out = args.root / args.topic / args.date
    driver = Driver(out, hooks or default_hooks())
    only = args.only.split(",") if getattr(args, "only", None) else None
    try:
        if args.command == "build":
            driver.build(args.backend, args.adapter)
        elif args.command == "correctness":
            driver.correctness(args.arms, only)
        elif args.command == "timing":
            driver.timing(only)
        else:
            driver.analyze()
        return 0
    except BenchError as error:
        print(f"bench.py: {error}", file=sys.stderr)
        return 1
    finally:
        gpu_evidence.write_checksums(out, recursive=True)


if __name__ == "__main__":
    sys.exit(main())
