#!/usr/bin/env python3
"""Statistics, per-process values and decision rules of the overhead benchmark round.

Every function is pure: it reads the per-process results it is given and returns plain data. The
numbers and rules come from the round's pre-registration
(docs/research/2026-10-18-metal4-overhead.md). `Tools/bench.py` reads the files and calls
`summary`.
"""

from __future__ import annotations

import random
import statistics
from collections.abc import Mapping, Sequence
from typing import Any


# Statistics: 20 processes per configuration, percentile bootstrap with 10,000 resamples and a fixed
# seed, bounds at sorted[250] and sorted[9749].
PROCESSES = 20
RESAMPLES = 10_000
SEED = 20261018
LOW_INDEX = 250
HIGH_INDEX = 9749

# Question, budget table: CPU tax at most 5%, GPU frame-time tax at most 1%.
CPU_BUDGET = 0.05
GPU_BUDGET = 0.01

# Decision rules, CPU: judged at K of 100 and above.
CPU_MIN_K = 100

# Workloads, CPU operations: RojoRHI calls per repetition (`c`).
CALLS_PER_REPETITION = {
    "C1": 3, "C2": 3, "C3": 2, "C4": 3, "C5": 1, "C6": 2, "C7": 1, "C8": 7, "C9": 1, "C10": 1,
}

# Workloads, CPU operations: N's native calls per repetition. C10 is absent: its counts are
# reported, not judged.
NATIVE_CALLS_PER_REPETITION = {
    "C1": 2, "C2": 2, "C3": 4, "C4": 2, "C5": 0, "C6": 2, "C7": 1, "C8": 12, "C9": 2,
}

# Metrics, native calls: the K pair of the difference, (1,000, 100) and for C9 (100, 10).
DEFAULT_K_PAIR = (1000, 100)
K_PAIRS = {"C9": (100, 10)}

# Decision rules, CPU: C1 to C10 are judged; C10 at maxCount 1 is reported only.
CPU_WORKLOADS = tuple(CALLS_PER_REPETITION)

# Decision rules, GPU: only the composed frame is judged.
GPU_WORKLOAD = "frame"

# Decision rules, A miss: the sentence on texture indexing the GPU miss carries as a reminder.
TEXTURE_INDEXING_REMINDER = (
    "Texture indexing is reopened (issue #30) only when the composed frame misses its GPU budget "
    "and investigation shows that texture indexing is the cause."
)


def median(values: Sequence[float]) -> float | None:
    """Return the median of the values, or None when there are none."""
    return statistics.median(values) if values else None


def process_value(samples: Sequence[float | None]) -> float | None:
    """Return the median of one arm's measured-frame values, skipping frames without a value."""
    return median([value for value in samples if value is not None])


def as_mapping(values: Mapping[Any, float | None] | Sequence[float | None]) -> dict[Any, float]:
    """Return the values keyed by process; a sequence is keyed by position. None is dropped."""
    items = values.items() if isinstance(values, Mapping) else enumerate(values)
    return {key: value for key, value in items if value is not None}


def interval(deltas: Sequence[float]) -> tuple[float, float]:
    """Return the 95% percentile bootstrap bounds of the median of the per-process deltas."""
    rng = random.Random(SEED)
    medians = sorted(
        statistics.median(rng.choices(deltas, k=len(deltas))) for _ in range(RESAMPLES)
    )
    return medians[LOW_INDEX], medians[HIGH_INDEX]


def compare(r_values: Mapping[Any, float | None] | Sequence[float | None],
            n_values: Mapping[Any, float | None] | Sequence[float | None]) -> dict | None:
    """Compare R with N over the processes both have; None when no process has both."""
    r_by_process = as_mapping(r_values)
    n_by_process = as_mapping(n_values)
    shared = sorted(set(r_by_process) & set(n_by_process))
    if not shared:
        return None
    deltas = [r_by_process[key] - n_by_process[key] for key in shared]
    low, high = interval(deltas)
    r_median = statistics.median(r_by_process[key] for key in shared)
    n_median = statistics.median(n_by_process[key] for key in shared)
    delta = statistics.median(deltas)
    return {
        "processes": len(shared),
        "rMedian": r_median,
        "nMedian": n_median,
        "delta": delta,
        "low": low,
        "high": high,
        "relative": delta / n_median if n_median else None,
    }


def count_list(counts: int | Sequence[int]) -> list[int]:
    """Return a count, or the counts of several frames and processes, as a list."""
    return [counts] if isinstance(counts, int) else list(counts)


def per_operation_count(count_at_hi: int | Sequence[int], count_at_lo: int | Sequence[int],
                        k_hi: int, k_lo: int) -> dict:
    """Return the native calls per repetition, (hi - lo) / (k_hi - k_lo).

    Counts given as a list come from several frames or processes. When they differ the value is
    flagged unstable and is the largest the counts allow: the highest at k_hi minus the lowest at
    k_lo.
    """
    hi = count_list(count_at_hi)
    lo = count_list(count_at_lo)
    unstable = len(set(hi)) > 1 or len(set(lo)) > 1
    return {"value": (max(hi) - min(lo)) / (k_hi - k_lo), "unstable": unstable}


def rule(judged: bool, miss: bool | None, reason: str, **details: Any) -> dict:
    """Build a rule result: judged or reported only, the verdict, and why."""
    return {"judged": judged, "miss": miss, "reason": reason, **details}


def excludes_zero(comparison: dict) -> bool:
    """Return whether the interval of the delta lies wholly on one side of zero."""
    return comparison["low"] > 0 or comparison["high"] < 0


def cpu_rule(workload: str, k: int | None, comparison: dict | None) -> dict:
    """Decision rules, CPU: C1 to C10 at K of 100 and above, never C10 at maxCount 1."""
    if workload not in CPU_WORKLOADS:
        return rule(False, None, "reported only")
    if k is None or k < CPU_MIN_K:
        return rule(False, None, f"K below {CPU_MIN_K}, reported only")
    if comparison is None:
        return rule(True, None, "not run")
    miss = excludes_zero(comparison) and comparison["delta"] > CPU_BUDGET * comparison["nMedian"]
    return rule(True, miss, "over budget" if miss else "within budget")


def gpu_rule(workload: str, comparison: dict | None) -> dict:
    """Decision rules, GPU: the composed frame only."""
    if workload != GPU_WORKLOAD:
        return rule(False, None, "reported only")
    if comparison is None:
        return rule(True, None, "not run")
    miss = excludes_zero(comparison) and comparison["delta"] > GPU_BUDGET * comparison["nMedian"]
    return rule(True, miss, "over budget" if miss else "within budget")


def count_rule(workload: str, r_count: dict | None, n_count: dict | None) -> dict:
    """Decision rules, counts: R's calls per repetition at most N's registered count plus c.

    The limit is the registered column, not the measured N. A measured N that differs from the
    column is a defect of the native arm: the rule cannot be judged and is not run.
    """
    registered = NATIVE_CALLS_PER_REPETITION.get(workload)
    if registered is None:
        return rule(False, None, "reported only")
    if r_count is None or n_count is None:
        return rule(True, None, "not run")
    if n_count["unstable"] or abs(n_count["value"] - registered) > 1e-9:
        return rule(True, None, f"not run: N makes {n_count['value']} calls per repetition, "
                                f"registered {registered}")
    limit = registered + CALLS_PER_REPETITION[workload]
    miss = r_count["value"] > limit
    return rule(True, miss, "over budget" if miss else "within budget", limit=limit)


def allocation_rule(comparison: dict | None) -> dict:
    """Decision rules, counts: R's allocations per frame at most N's, as medians of processes.

    The note says "any workload"; this rule is judged per configuration, which is stricter.
    """
    if comparison is None:
        return rule(True, None, "not run")
    miss = comparison["rMedian"] > comparison["nMedian"]
    return rule(True, miss, "over budget" if miss else "within budget")


def pipeline_rule(created_after_warmup: Sequence[int] | None) -> dict:
    """Decision rules, counts: no pipeline created after warm-up in any R process."""
    if not created_after_warmup:
        return rule(True, None, "not run")
    miss = any(created > 0 for created in created_after_warmup)
    return rule(True, miss, "pipeline created after warm-up" if miss else "none created")


def arm_of(process: dict, name: str) -> dict | None:
    """Return the arm of a process result when it ran and succeeded."""
    arm = process.get("arms", {}).get(name)
    return arm if arm and arm.get("status") == "ok" else None


def frame_values(process: dict, name: str, field: str, divisor: int = 1) -> list[float | None]:
    """Return one field of an arm per measured frame, divided by `divisor`."""
    arm = arm_of(process, name)
    if arm is None:
        return []
    return [None if value is None else value / divisor for value in arm.get(field) or []]


def process_values(processes: Mapping[int, dict], name: str, field: str,
                   divisor: int = 1) -> dict[int, float]:
    """Return the process value of an arm's field for every process that has one."""
    values = {key: process_value(frame_values(process, name, field, divisor))
              for key, process in processes.items()}
    return {key: value for key, value in values.items() if value is not None}


def pipelines_after_warmup(processes: Mapping[int, dict]) -> dict | None:
    """Return R's pipelines created and native create calls after warm-up, per process."""
    created: list[int] = []
    native: list[int] = []
    for process in processes.values():
        arm = arm_of(process, "R")
        counters = arm.get("counters") if arm else None
        if not counters:
            return None
        after, end = counters["afterWarmup"], counters["end"]
        created.append(end["pipelinesCreated"] - after["pipelinesCreated"])
        native.append(end["nativeCreateCalls"] - after["nativeCreateCalls"])
    return {"created": created, "nativeCreateCalls": native} if created else None


def flat_counts(processes: Mapping[int, dict], name: str) -> list[int]:
    """Return an arm's native calls of every measured frame of every process."""
    counts: list[int] = []
    for process in processes.values():
        counts.extend(value for value in frame_values(process, name, "nativeCalls")
                      if value is not None)
    return counts


def complete(processes: Mapping[int, dict] | None) -> bool:
    """Return whether every one of the round's processes has a kept result."""
    return processes is not None and all(key in processes for key in range(PROCESSES))


def workload_counts(workload: str, configurations: Sequence[dict],
                    processes: Mapping[str, Mapping[int, dict]]) -> dict:
    """Return R's and N's native calls per repetition, and the rule, for one workload."""
    k_hi, k_lo = K_PAIRS.get(workload, DEFAULT_K_PAIR)
    by_k = {config["params"].get("K"): config["id"]
            for config in configurations if config["workload"] == workload}
    result: dict[str, Any] = {}
    for name in ("R", "N"):
        pair = []
        for k in (k_hi, k_lo):
            held = processes.get(by_k.get(k, ""))
            pair.append(flat_counts(held, name) if complete(held) else [])
        result[name] = per_operation_count(pair[0], pair[1], k_hi, k_lo) if all(pair) else None
    result["c"] = CALLS_PER_REPETITION.get(workload)
    result["registeredN"] = NATIVE_CALLS_PER_REPETITION.get(workload)
    result["rule"] = count_rule(workload, result["R"], result["N"])
    return result


def configuration_summary(config: dict, held: Mapping[int, dict] | None, notrun: str | None,
                          workload_passed: bool | None) -> dict:
    """Return the reported values, comparisons and rule results of one configuration."""
    workload, k = config["workload"], config["params"].get("K")
    entry: dict[str, Any] = {"id": config["id"], "workload": workload, "params": config["params"]}
    if notrun is not None:
        reason = notrun
    elif workload_passed is not True:
        reason = "correctness gate not passed"
    elif not complete(held):
        reason = f"{len(held or {})} of {PROCESSES} processes"
    else:
        reason = None
    if reason is not None:
        entry["ran"] = False
        entry["reason"] = reason
        entry["rules"] = {
            "cpu": cpu_rule(workload, k, None),
            "gpu": gpu_rule(workload, None),
            "allocations": allocation_rule(None),
            "pipelines": pipeline_rule(None),
        }
        return entry
    cpu = compare(process_values(held, "R", "cpuNs", k or 1),
                  process_values(held, "N", "cpuNs", k or 1))
    gpu = compare(process_values(held, "R", "gpuNs"), process_values(held, "N", "gpuNs"))
    allocations = compare(process_values(held, "R", "allocations"),
                          process_values(held, "N", "allocations"))
    pipelines = pipelines_after_warmup(held)
    entry["ran"] = True
    entry["cpuPerOperationNs"] = cpu
    entry["gpuNs"] = gpu
    entry["gpuNsF"] = compare(process_values(held, "F", "gpuNs"),
                              process_values(held, "N", "gpuNs"))
    entry["allocations"] = allocations
    entry["cpuAllocationsR"] = median(list(process_values(held, "R", "cpuAllocations").values()))
    entry["pipelinesAfterWarmup"] = pipelines
    entry["rules"] = {
        "cpu": cpu_rule(workload, k, cpu),
        "gpu": gpu_rule(workload, gpu),
        "allocations": allocation_rule(allocations),
        "pipelines": pipeline_rule(pipelines["created"] if pipelines else None),
    }
    return entry


def decisions(configurations: Sequence[dict], workloads: Mapping[str, dict]) -> dict:
    """List every miss and every judged rule whose configuration did not run."""
    misses: list[dict] = []
    not_run: list[dict] = []

    def note(subject: str, name: str, result: dict) -> None:
        if not result["judged"]:
            return
        if result["miss"]:
            misses.append({"subject": subject, "rule": name, "reason": result["reason"]})
        elif result["miss"] is None:
            not_run.append({"subject": subject, "rule": name, "reason": result["reason"]})

    for entry in configurations:
        for name, result in entry["rules"].items():
            note(entry["id"], name, result)
    for workload, counts in workloads.items():
        note(workload, "counts", counts["rule"])
    return {"misses": misses, "notRun": not_run}


def summary(configurations: Sequence[dict], processes: Mapping[str, Mapping[int, dict]],
            notruns: Mapping[str, str], correctness: Mapping[str, dict]) -> dict:
    """Return the round's summary: values and rule results per configuration, then the verdict.

    `processes` maps a configuration id to its kept results keyed by process number, `notruns`
    maps an id to the reason it was not run, and `correctness` maps a workload to its gate result.
    """
    entries = []
    for config in configurations:
        gate = correctness.get(config["workload"])
        entries.append(configuration_summary(
            config, processes.get(config["id"]), notruns.get(config["id"]),
            None if gate is None else gate.get("passed")))
    names = list(dict.fromkeys(config["workload"] for config in configurations))
    workloads = {name: workload_counts(name, configurations, processes)
                 for name in names if name in CALLS_PER_REPETITION or name == "C10m1"}
    found = decisions(entries, workloads)
    gpu_missed = any(item["rule"] == "gpu" for item in found["misses"])
    if found["misses"]:
        verdict = "budget missed"
    elif found["notRun"]:
        verdict = "incomplete"
    else:
        verdict = "within budget"
    result: dict[str, Any] = {
        "configurations": entries,
        "workloads": workloads,
        "decisions": found,
        "verdict": verdict,
    }
    if gpu_missed:
        result["textureIndexing"] = TEXTURE_INDEXING_REMINDER
    return result


def number(value: float | None, digits: int = 1) -> str:
    """Format a number for the report, "-" when absent."""
    return "-" if value is None else f"{value:.{digits}f}"


def percent(value: float | None) -> str:
    """Format a relative delta as a signed percentage."""
    return "-" if value is None else f"{value * 100:+.2f}%"


def verdict_word(result: dict) -> str:
    """Return the one-word state of a rule result."""
    if not result["judged"]:
        return "reported"
    return "not run" if result["miss"] is None else ("MISS" if result["miss"] else "pass")


def comparison_cells(comparison: dict | None, digits: int = 1) -> list[str]:
    """Return R median, N median, delta, interval and relative delta as table cells."""
    if comparison is None:
        return ["-"] * 5
    return [number(comparison["rMedian"], digits), number(comparison["nMedian"], digits),
            number(comparison["delta"], digits),
            f"[{number(comparison['low'], digits)}, {number(comparison['high'], digits)}]",
            percent(comparison["relative"])]


def table(title: str, header: list[str], rows: list[list[str]]) -> list[str]:
    """Render a titled, column-aligned text table."""
    widths = [max(len(row[i]) for row in [header, *rows]) for i in range(len(header))]
    lines = ["", title]
    for row in [header, *rows]:
        lines.append("  ".join(cell.ljust(width) for cell, width in zip(row, widths)).rstrip())
    return lines


def format_report(result: Mapping[str, Any]) -> str:
    """Render the summary as one readable table per rule, then the decisions."""
    entries = result["configurations"]
    comparison_header = ["R", "N", "delta", "95% interval", "relative"]
    lines = [f"verdict: {result['verdict']}"]
    lines += table("CPU per operation, ns (miss: interval excludes zero and delta over 5%)",
                   ["id", "rule", *comparison_header],
                   [[e["id"], verdict_word(e["rules"]["cpu"]),
                     *comparison_cells(e.get("cpuPerOperationNs"), 2)] for e in entries])
    lines += table("GPU time, ns (judged on the composed frame; over 1%)",
                   ["id", "rule", *comparison_header],
                   [[e["id"], verdict_word(e["rules"]["gpu"]), *comparison_cells(e.get("gpuNs"), 0)]
                    for e in entries if e.get("gpuNs") or e["rules"]["gpu"]["judged"]])
    lines += table("GPU time of F against N, ns (reported)",
                   ["id", "", *comparison_header],
                   [[e["id"], "reported", *comparison_cells(e["gpuNsF"], 0)]
                    for e in entries if e.get("gpuNsF")])
    lines += table("Native calls per repetition (miss: R above N plus c)",
                   ["workload", "rule", "R", "N", "limit", "c", "unstable"],
                   [[name, verdict_word(w["rule"]),
                     number(w["R"]["value"], 3) if w["R"] else "-",
                     number(w["N"]["value"], 3) if w["N"] else "-",
                     number(w["rule"].get("limit"), 3),
                     number(w["c"], 0),
                     "yes" if any(w[a] and w[a]["unstable"] for a in ("R", "N")) else "no"]
                    for name, w in result["workloads"].items()])
    lines += table("Allocations per frame (miss: R median above N)",
                   ["id", "rule", *comparison_header, "R cpuAllocations"],
                   [[e["id"], verdict_word(e["rules"]["allocations"]),
                     *comparison_cells(e.get("allocations"), 1),
                     number(e.get("cpuAllocationsR"), 1)] for e in entries])
    lines += table("Pipelines created after warm-up (miss: any)",
                   ["id", "rule", "created", "nativeCreateCalls"],
                   [[e["id"], verdict_word(e["rules"]["pipelines"]),
                     ",".join(map(str, set(e["pipelinesAfterWarmup"]["created"])))
                     if e.get("pipelinesAfterWarmup") else "-",
                     ",".join(map(str, set(e["pipelinesAfterWarmup"]["nativeCreateCalls"])))
                     if e.get("pipelinesAfterWarmup") else "-"] for e in entries])
    found = result["decisions"]
    lines += ["", "decisions"]
    lines += [f"  miss: {item['subject']} {item['rule']}: {item['reason']}"
              for item in found["misses"]] or ["  no misses"]
    lines += [f"  not run: {item['subject']} {item['rule']}: {item['reason']}"
              for item in found["notRun"]]
    if "textureIndexing" in result:
        lines += ["", result["textureIndexing"]]
    return "\n".join(lines) + "\n"
