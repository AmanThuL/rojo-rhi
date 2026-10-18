from __future__ import annotations

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from Tools import bench


CONFIGS = [
    {"id": "C1-k100", "workload": "C1", "params": {"K": 100}, "arms": ["R", "N"]},
    {"id": "C1-k1000", "workload": "C1", "params": {"K": 1000}, "arms": ["R", "N"]},
    {"id": "frame", "workload": "frame", "params": {}, "arms": ["R", "N"]},
]


def arm(k: int, calls: int, status: str = "ok") -> dict:
    return {"status": status, "error": "", "cpuNs": [100.0 * k] * 3, "gpuNs": [None] * 3,
            "allocations": [4] * 3, "nativeCalls": [calls] * 3, "nativeBarriers": [0] * 3,
            "cpuAllocations": None, "counters": None, "frameCounters": None,
            "outputs": [{"name": "target", "bytes": 4, "sha256": "x"}]}


class FakeBench:
    """Stands in for subprocess: answers the harness commands and records how it was called."""

    def __init__(self, n_calls_offset: int = 0, unequal: bool = False, stderr: str = "",
                 thermal=None, exit_code: int = 0, failed_arm: str | None = None) -> None:
        self.calls: list[tuple[list[str], dict | None]] = []
        self.counts: dict[str, int] = {}
        self.n_calls_offset = n_calls_offset
        self.unequal = unequal
        self.stderr = stderr
        self.thermal = thermal or (lambda id_, previous: "nominal")
        self.exit_code = exit_code
        self.failed_arm = failed_arm

    def __call__(self, argv, cwd=None, env=None) -> subprocess.CompletedProcess:
        self.calls.append((list(argv), env))
        options = dict(zip(argv[2:], argv[3:]))
        id_ = argv[2]
        k = next((c["params"].get("K", 1) for c in CONFIGS if c["id"] == id_), 1)
        previous = self.counts.get(id_, 0)
        self.counts[id_] = previous + 1
        arms = {}
        for name in options["--arms"].split(","):
            offset = self.n_calls_offset if name == "N" else 0
            arms[name] = arm(k, 2 * k + 7 + offset, "failed" if name == self.failed_arm else "ok")
        state = self.thermal(id_, previous)
        result = {"schema": 1, "id": id_, "mode": options["--mode"], "arms": arms,
                  "thermalStart": state, "thermalEnd": "nominal",
                  "order": list(options["--order"]),
                  "outputsEqual": True if {"R", "N"} <= set(arms) else None}
        if self.exit_code == 0:
            Path(options["--out"]).write_text(json.dumps(result))
        if "--dump" in options:
            dump = Path(options["--dump"])
            dump.mkdir(parents=True, exist_ok=True)
            for name in arms:
                data = b"BAD!" if self.unequal and name == "R" else b"good"
                (dump / f"{id_}.{name}.target.bin").write_bytes(data)
        return subprocess.CompletedProcess(argv, self.exit_code, "", self.stderr)


class DriverCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        quiet = contextlib.redirect_stdout(io.StringIO())
        quiet.__enter__()
        self.addCleanup(quiet.__exit__, None, None, None)
        self.out = Path(self.temp.name) / "t2" / "2026-10-18"
        self.out.mkdir(parents=True)
        self.sleeps: list[float] = []
        self.power_state = "AC Power"
        self.fake = FakeBench()
        self.write_build("metal4")

    def write_build(self, backend: str) -> None:
        (self.out / "configurations.json").write_text(json.dumps(CONFIGS))
        (self.out / "bench-environment.json").write_text(json.dumps({"backend": backend}))

    def driver(self, fake: FakeBench | None = None) -> bench.Driver:
        hooks = bench.Hooks(
            run=fake or self.fake, sleep=self.sleeps.append, clock=lambda: 0.0,
            power=lambda: self.power_state, environment=lambda: {"device": "test"},
            base_env={"PATH": "/bin", "MTL_DEBUG_LAYER": "1", "METAL_DEVICE_WRAPPER_TYPE": "1"})
        return bench.Driver(self.out, hooks, Path(self.temp.name) / "repo")


class TimingTests(DriverCase):
    def passing(self, *workloads: str) -> None:
        (self.out / "correctness").mkdir(exist_ok=True)
        (self.out / "correctness" / "summary.json").write_text(
            json.dumps({w: {"passed": True, "failures": []} for w in workloads}))

    def timing_calls(self) -> list[tuple[str, str, str]]:
        runs = [argv for argv, _ in self.fake.calls if argv[1] == "run"]
        return [(a[2], a[a.index("--order") + 1], a[a.index("--arms") + 1]) for a in runs]

    def test_schedule_interleaves_configurations_and_alternates_order(self) -> None:
        self.passing("C1")
        self.driver().timing(only=None)
        calls = self.timing_calls()
        self.assertEqual(calls[:8], [
            ("C1-k100", "RN", "R,N"), ("C1-k1000", "RN", "R,N"),
            ("C1-k100", "NR", "R,N"), ("C1-k1000", "NR", "R,N"),
            ("C1-k100", "RN", "R,N"), ("C1-k1000", "RN", "R,N"),
            ("C1-k100", "NR", "R,N"), ("C1-k1000", "NR", "R,N")])
        self.assertEqual(len(calls), 40)
        self.assertTrue((self.out / "timing" / "C1-k1000-p19.json").exists())

    def test_workloads_whose_correctness_did_not_pass_are_skipped(self) -> None:
        self.passing()
        self.driver().timing(only=None)
        self.assertEqual(self.timing_calls(), [])

    def test_kept_results_carry_power_and_attempt(self) -> None:
        self.passing("C1")
        self.driver().timing(only=["C1"])
        kept = json.loads((self.out / "timing" / "C1-k100-p00.json").read_text())
        self.assertEqual((kept["power"], kept["attempt"]), ("AC Power", 1))

    def test_only_filters_workloads(self) -> None:
        self.passing("C1", "frame")
        self.driver().timing(only=["frame"])
        self.assertEqual({c[0] for c in self.timing_calls()}, {"frame"})

    def test_existing_results_are_skipped(self) -> None:
        self.passing("C1")
        (self.out / "timing").mkdir()
        (self.out / "timing" / "C1-k100-p00.json").write_text("{}")
        self.driver().timing(only=["C1"])
        self.assertEqual(len(self.timing_calls()), 39)
        self.assertEqual((self.out / "timing" / "C1-k100-p00.json").read_text(), "{}")

    def test_a_thermal_discard_is_renamed_rested_and_rerun(self) -> None:
        self.passing("C1")
        fake = FakeBench(thermal=lambda id_, previous: "fair" if (id_, previous) == ("C1-k100", 0)
                         else "nominal")
        self.driver(fake).timing(only=["C1"])
        timing = self.out / "timing"
        self.assertTrue((timing / "C1-k100-p00.discarded1.json").exists())
        self.assertEqual(json.loads((timing / "C1-k100-p00.json").read_text())["attempt"], 2)
        self.assertEqual(self.sleeps, [120])

    def test_the_fourth_discard_writes_notrun_and_skips_the_rest(self) -> None:
        self.passing("C1")
        fake = FakeBench(thermal=lambda id_, previous: "serious" if id_ == "C1-k100" else "nominal")
        self.driver(fake).timing(only=["C1"])
        timing = self.out / "timing"
        for attempt in (1, 2, 3, 4):
            self.assertTrue((timing / f"C1-k100-p00.discarded{attempt}.json").exists())
        notrun = json.loads((timing / "C1-k100.notrun.json").read_text())
        self.assertIn("serious", notrun["reason"])
        self.assertEqual(self.sleeps, [120, 120, 120])
        self.assertEqual(fake.counts["C1-k100"], 4)
        self.assertEqual(fake.counts["C1-k1000"], 20)

    def test_a_resumed_attempt_rests_before_it_runs(self) -> None:
        self.passing("C1")
        (self.out / "timing").mkdir()
        (self.out / "timing" / "C1-k100-p00.discarded1.json").write_text("{}")
        self.driver(FakeBench()).timing(only=["C1"])
        self.assertEqual(self.sleeps, [120])
        kept = json.loads((self.out / "timing" / "C1-k100-p00.json").read_text())
        self.assertEqual(kept["attempt"], 2)

    def test_a_resumed_discard_counts_toward_the_four_attempts(self) -> None:
        self.passing("C1")
        (self.out / "timing").mkdir()
        for attempt in (1, 2, 3):
            (self.out / "timing" / f"C1-k100-p00.discarded{attempt}.json").write_text("{}")
        fake = FakeBench(thermal=lambda id_, previous: "fair" if id_ == "C1-k100" else "nominal")
        self.driver(fake).timing(only=["C1"])
        self.assertEqual(self.sleeps, [120])
        self.assertEqual(fake.counts["C1-k100"], 1)
        self.assertTrue((self.out / "timing" / "C1-k100.notrun.json").exists())

    def test_battery_power_is_refused_before_any_process(self) -> None:
        self.passing("C1")
        self.power_state = "Battery Power"
        with self.assertRaisesRegex(bench.BenchError, "battery"):
            self.driver().timing(only=None)
        self.assertEqual(self.timing_calls(), [])

    def test_a_switch_to_battery_mid_run_stops_before_the_next_process(self) -> None:
        self.passing("C1")
        readings = iter(["AC Power"] * 3)
        hooks = bench.Hooks(
            run=self.fake, sleep=self.sleeps.append, clock=lambda: 0.0,
            power=lambda: next(readings, "Battery Power"), environment=lambda: {},
            base_env={})
        with self.assertRaisesRegex(bench.BenchError, "battery"):
            bench.Driver(self.out, hooks, Path(self.temp.name)).timing(only=["C1"])
        self.assertEqual(len(self.timing_calls()), 3)
        self.assertFalse((self.out / "timing" / "C1-k1000-p01.json").exists())

    def test_a_null_build_is_refused(self) -> None:
        self.passing("C1")
        self.write_build("null")
        with self.assertRaisesRegex(bench.BenchError, "Null"):
            self.driver().timing(only=None)
        self.assertEqual(self.timing_calls(), [])

    def test_malloc_variables_are_stripped_and_recorded(self) -> None:
        self.passing("C1")
        hooks = bench.Hooks(
            run=self.fake, sleep=self.sleeps.append, clock=lambda: 0.0, power=lambda: "AC Power",
            environment=lambda: {}, base_env={"PATH": "/bin", "MallocStackLogging": "1",
                                              "MallocScribble": "1", "MTL_X": "1"})
        driver = bench.Driver(self.out, hooks, Path(self.temp.name))
        driver.timing(only=["C1"])
        driver.correctness(arms="R,N", only=["C1"])
        for _, env in self.fake.calls:
            self.assertFalse([key for key in env if key.startswith(("Malloc", "MTL_", "METAL_"))
                              and key != "MTL_DEBUG_LAYER"])
        record = json.loads((self.out / "bench-environment.json").read_text())
        recorded = record["strippedEnvironment"]
        self.assertEqual(recorded, ["MTL_X", "MallocScribble", "MallocStackLogging"])

    def test_a_failed_arm_stops_with_the_log_path(self) -> None:
        self.passing("C1")
        fake = FakeBench(failed_arm="N")
        with self.assertRaisesRegex(bench.BenchError, r"C1-k100-p00\.log"):
            self.driver(fake).timing(only=None)
        self.assertEqual(len(fake.calls), 1)
        self.assertFalse((self.out / "timing" / "C1-k100-p00.json").exists())

    def test_a_non_zero_exit_stops_and_leaves_no_result(self) -> None:
        self.passing("C1")
        fake = FakeBench(exit_code=3, stderr="boom\n")
        with self.assertRaisesRegex(bench.BenchError, r"C1-k100-p00\.log"):
            self.driver(fake).timing(only=None)
        self.assertEqual((self.out / "timing" / "C1-k100-p00.log").read_text(), "boom\n")
        self.assertEqual(sorted(p.name for p in (self.out / "timing").glob("*.json")), [])

    def test_timing_strips_the_metal_environment_and_uses_the_release_binary(self) -> None:
        self.passing("C1")
        self.driver().timing(only=["C1"])
        argv, env = self.fake.calls[0]
        self.assertEqual(env, {"PATH": "/bin"})
        self.assertEqual(argv[0], str(self.out / "bin" / "release" / "RojoRHIBench"))
        self.assertNotIn("--validation", argv)


class CorrectnessTests(DriverCase):
    def summary(self) -> dict:
        return json.loads((self.out / "correctness" / "summary.json").read_text())

    def test_equal_dumps_pass_and_both_builds_run(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertEqual(self.summary()["C1"], {"passed": True, "failures": [], "gate": "compared"})
        builds = [argv[0].split("/")[-2] for argv, _ in self.fake.calls]
        self.assertEqual(sorted(set(builds)), ["debug", "release"])
        self.assertTrue((self.out / "correctness" / "C1-k100.release.json").exists())
        dump = self.out / "correctness" / "dumps" / "debug" / "C1-k100.R.target.bin"
        self.assertTrue(dump.exists())

    def test_environments_and_validation_flag_per_build(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        for argv, env in self.fake.calls:
            if "/release/" in argv[0]:
                self.assertEqual(env, {"PATH": "/bin"})
                self.assertNotIn("--validation", argv)
            else:
                self.assertEqual(env, {"PATH": "/bin", "MTL_DEBUG_LAYER": "1"})
                self.assertIn("--validation", argv)

    def test_n_only_runs_do_not_validate_r(self) -> None:
        self.driver().correctness(arms="N", only=["C1"])
        self.assertTrue(all("--validation" not in argv for argv, _ in self.fake.calls))

    def test_a_foreign_environment_file_does_not_break_correctness(self) -> None:
        (self.out / "environment.json").write_text(json.dumps({"machine": "arm64"}))
        self.driver().correctness(arms="R,N", only=["C1"])
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertTrue(self.summary()["C1"]["passed"])
        self.assertEqual(json.loads((self.out / "environment.json").read_text()),
                         {"machine": "arm64"})

    def test_a_single_arm_run_is_a_smoke_that_timing_does_not_accept(self) -> None:
        for arms in ("N", "R"):
            with self.subTest(arms=arms):
                self.driver().correctness(arms=arms, only=["C1"])
                entry = self.summary()["C1"]
                self.assertEqual((entry["passed"], entry["gate"]), (False, "smoke"))
        self.fake.calls.clear()
        self.driver().timing(only=["C1"])
        self.assertEqual(self.fake.calls, [])

    def test_a_compared_run_records_the_gate(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertEqual(self.summary()["C1"]["gate"], "compared")

    def test_a_full_run_replaces_the_results_of_an_earlier_smoke(self) -> None:
        self.driver().correctness(arms="N", only=["C1"])
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertTrue(self.summary()["C1"]["passed"])

    def test_unequal_dumps_fail_the_workload(self) -> None:
        self.driver(FakeBench(unequal=True)).correctness(arms="R,N", only=["C1"])
        entry = self.summary()["C1"]
        self.assertFalse(entry["passed"])
        self.assertTrue(any("target" in failure for failure in entry["failures"]))

    def test_a_stray_standard_error_line_fails(self) -> None:
        self.driver(FakeBench(stderr="warning: something\n")).correctness(arms="R,N", only=["C1"])
        self.assertFalse(self.summary()["C1"]["passed"])

    def test_the_validation_banner_alone_is_allowed(self) -> None:
        fake = FakeBench(stderr="2026 Metal API Validation Enabled\n")
        self.driver(fake).correctness(arms="R,N", only=["C1"])
        self.assertTrue(self.summary()["C1"]["passed"])

    def test_n_counts_off_the_registered_column_fail(self) -> None:
        # N makes 2 calls per repetition; a 3rd per repetition is not the registered minimum.
        fake = FakeBench()
        original = fake.__call__

        def off(argv, cwd=None, env=None):
            result = original(argv, cwd, env)
            out = Path(argv[argv.index("--out") + 1])
            data = json.loads(out.read_text())
            k = 100 if "k100" in argv[2] else 1000
            data["arms"]["N"]["nativeCalls"] = [3 * k + 7] * 3
            out.write_text(json.dumps(data))
            return result

        self.driver(off).correctness(arms="R,N", only=["C1"])
        entry = self.summary()["C1"]
        self.assertFalse(entry["passed"])
        self.assertTrue(any("native calls" in failure for failure in entry["failures"]))

    def count_gate(self, edit) -> dict:
        original = self.fake.__call__

        def patched(argv, cwd=None, env=None):
            result = original(argv, cwd, env)
            out = Path(argv[argv.index("--out") + 1])
            data = json.loads(out.read_text())
            edit(argv[2], data)
            out.write_text(json.dumps(data))
            return result

        self.driver(patched).correctness(arms="R,N", only=["C1"])
        return self.summary()["C1"]

    def test_an_empty_native_call_list_fails_the_count_gate(self) -> None:
        entry = self.count_gate(lambda id_, data: data["arms"]["N"].update(nativeCalls=[]))
        self.assertFalse(entry["passed"])
        self.assertTrue(any("native call count" in f for f in entry["failures"]))

    def test_a_null_native_call_entry_fails_the_count_gate(self) -> None:
        entry = self.count_gate(lambda id_, data: data["arms"]["N"].update(nativeCalls=[None] * 3))
        self.assertFalse(entry["passed"])

    def test_a_missing_k_pair_configuration_fails_the_count_gate(self) -> None:
        (self.out / "configurations.json").write_text(json.dumps(CONFIGS[:1] + CONFIGS[2:]))
        self.driver().correctness(arms="R,N", only=["C1"])
        entry = self.summary()["C1"]
        self.assertFalse(entry["passed"])
        self.assertTrue(any("K 1000" in f for f in entry["failures"]))

    def test_registered_counts_pass_for_the_registered_minimum(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertEqual(self.summary()["C1"]["failures"], [])

    def test_r_is_refused_on_a_null_build(self) -> None:
        self.write_build("null")
        with self.assertRaisesRegex(bench.BenchError, "--arms N"):
            self.driver().correctness(arms="R,N", only=None)
        self.assertEqual(self.fake.calls, [])

    def test_n_only_runs_on_a_null_build(self) -> None:
        self.write_build("null")
        self.driver().correctness(arms="N", only=["C1"])
        self.assertEqual(self.summary()["C1"]["gate"], "smoke")

    def test_existing_results_are_skipped(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        count = len(self.fake.calls)
        self.driver().correctness(arms="R,N", only=["C1"])
        self.assertEqual(len(self.fake.calls), count)

    def test_a_harness_failure_is_a_failure_of_the_workload(self) -> None:
        failing = FakeBench(exit_code=2, stderr="no device\n")
        self.driver(failing).correctness(arms="R,N", only=["C1"])
        self.assertFalse(self.summary()["C1"]["passed"])

    def test_other_workloads_keep_their_earlier_results(self) -> None:
        self.driver().correctness(arms="R,N", only=["C1"])
        self.driver().correctness(arms="R,N", only=["frame"])
        self.assertEqual(sorted(self.summary()), ["C1", "frame"])


class BuildAndMainTests(DriverCase):
    def test_build_copies_both_modes_and_reconfigures_to_null_debug(self) -> None:
        repo = Path(self.temp.name) / "repo"
        for mode in ("release", "debug"):
            tree = repo / "build" / "macosx" / "arm64" / mode / "rojorhi-bench"
            (tree / "Shaders").mkdir(parents=True)
            (tree / "RojoRHIBench").write_text("exe")
            (tree / "Shaders" / "a.metallib").write_text("lib")
        steps: list[list[str]] = []

        def run(argv, cwd=None, env=None):
            steps.append(list(argv))
            text = ""
            if argv[1:2] == ["list"]:
                text = "".join(json.dumps(c) + "\n" for c in CONFIGS)
            if argv[1:2] == ["environment"]:
                Path(argv[2]).write_text(json.dumps({"device": "d", "metal4": True}))
            return subprocess.CompletedProcess(argv, 0, text, "")

        hooks = bench.Hooks(run=run, sleep=lambda s: None, clock=lambda: 0.0,
                            power=lambda: "AC Power", environment=lambda: {"device": "test"},
                            base_env={})
        bench.Driver(self.out, hooks, repo).build("metal4")
        xmake = [" ".join(s) for s in steps if s[0] == "xmake"]
        self.assertEqual(xmake, [
            "xmake f -m release -y --rojorhi_backend=metal4", "xmake build RojoRHIBench",
            "xmake f -m debug -y --rojorhi_backend=metal4", "xmake build RojoRHIBench",
            "xmake f -m debug -y --rojorhi_backend=null"])
        self.assertTrue((self.out / "bin" / "debug" / "Shaders" / "a.metallib").exists())
        environment = json.loads((self.out / "bench-environment.json").read_text())
        found = (environment["backend"], environment["power"], environment["bench"]["metal4"])
        self.assertEqual(found, ("metal4", "AC Power", True))
        self.assertEqual(len(json.loads((self.out / "configurations.json").read_text())), 3)

    def test_a_rejected_configure_fails_with_the_message(self) -> None:
        def run(argv, cwd=None, env=None):
            return subprocess.CompletedProcess(argv, 1, "", "error: metal4 not supported\n")

        hooks = bench.Hooks(run=run, sleep=lambda s: None, clock=lambda: 0.0,
                            power=lambda: "AC Power", environment=lambda: {}, base_env={})
        with self.assertRaisesRegex(bench.BenchError, "metal4 not supported"):
            bench.Driver(self.out, hooks, Path(self.temp.name)).build("metal4")

    def test_every_command_rewrites_recursive_checksums(self) -> None:
        hooks = bench.Hooks(run=self.fake, sleep=lambda s: None, clock=lambda: 0.0,
                            power=lambda: "AC Power", environment=lambda: {}, base_env={})
        (self.out / "timing").mkdir()
        (self.out / "timing" / "x.json").write_text("{}")
        code = bench.main(["--root", str(self.out.parents[1]), "--topic", "t2",
                           "--date", "2026-10-18", "analyze"], hooks)
        self.assertEqual(code, 0)
        sums = (self.out / "SHA256SUMS").read_text()
        self.assertIn("  timing/x.json", sums)
        self.assertIn("  summary.json", sums)
        self.assertTrue((self.out / "analysis.txt").exists())

    def test_analyze_reads_kept_results_only(self) -> None:
        (self.out / "timing").mkdir()
        (self.out / "timing" / "C1-k100-p00.discarded1.json").write_text("{not json")
        (self.out / "timing" / "C1-k100.notrun.json").write_text(json.dumps({"reason": "hot"}))
        hooks = bench.Hooks(run=self.fake, sleep=lambda s: None, clock=lambda: 0.0,
                            power=lambda: "AC Power", environment=lambda: {}, base_env={})
        bench.Driver(self.out, hooks, Path(self.temp.name)).analyze()
        result = json.loads((self.out / "summary.json").read_text())
        entry = {e["id"]: e for e in result["configurations"]}["C1-k100"]
        self.assertEqual(entry["reason"], "hot")

    def test_main_reports_a_driver_error_on_standard_error(self) -> None:
        self.write_build("null")
        hooks = bench.Hooks(run=self.fake, sleep=lambda s: None, clock=lambda: 0.0,
                            power=lambda: "AC Power", environment=lambda: {}, base_env={})
        code = bench.main(["--root", str(self.out.parents[1]), "--topic", "t2",
                           "--date", "2026-10-18", "timing"], hooks)
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
