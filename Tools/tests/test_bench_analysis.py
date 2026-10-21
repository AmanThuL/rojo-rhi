from __future__ import annotations

import random
import statistics
import unittest

from Tools import bench_analysis as analysis


def cmp(r: float, n: float, delta: float, low: float, high: float) -> dict:
    return {"rMedian": r, "nMedian": n, "delta": delta, "low": low, "high": high,
            "relative": delta / n}


def arm(cpu=None, gpu=None, allocations=None, calls=None, counters=None, cpu_allocations=None):
    return {"status": "ok", "cpuNs": cpu, "gpuNs": gpu, "allocations": allocations,
            "nativeCalls": calls, "cpuAllocations": cpu_allocations, "counters": counters}


def counters(created_after: int, created_end: int) -> dict:
    return {"afterWarmup": {"pipelinesCreated": created_after, "nativeCreateCalls": 5},
            "end": {"pipelinesCreated": created_end, "nativeCreateCalls": 5}}


class StatisticsTests(unittest.TestCase):
    def test_process_value_is_the_median_of_frames_without_gaps(self) -> None:
        self.assertEqual(analysis.process_value([5, None, 1, 3]), 3)

    def test_reported_value_is_the_median_of_process_medians(self) -> None:
        values = [analysis.process_value(s) for s in ([1, 1, 100], [2, 2, 2], [3, 3, 3])]
        result = analysis.compare(values, [1, 1, 1])
        self.assertEqual(result["rMedian"], 2)

    def test_compare_pairs_values_by_process(self) -> None:
        result = analysis.compare({0: 10, 1: 20, 2: 30}, {2: 29, 1: 18, 5: 99})
        self.assertEqual(result["processes"], 2)
        self.assertEqual(result["delta"], 1.5)
        self.assertEqual(result["nMedian"], 23.5)

    def test_relative_is_the_delta_over_n(self) -> None:
        result = analysis.compare([11, 11, 11], [10, 10, 10])
        self.assertAlmostEqual(result["relative"], 0.1)

    def test_compare_without_shared_processes_is_none(self) -> None:
        self.assertIsNone(analysis.compare({0: 1}, {1: 1}))

    def test_interval_is_deterministic(self) -> None:
        r = [10 + (i * 7) % 5 for i in range(20)]
        n = [9 + (i * 3) % 4 for i in range(20)]
        self.assertEqual(analysis.compare(r, n), analysis.compare(r, n))

    def test_interval_does_not_depend_on_other_calls(self) -> None:
        r, n = list(range(20)), [0] * 20
        first = analysis.compare(r, n)
        random.seed(1)
        random.random()
        analysis.compare([1, 2, 3], [3, 2, 1])
        self.assertEqual(analysis.compare(r, n), first)

    def test_interval_bounds_of_a_hand_built_list(self) -> None:
        # Ten zeros and ten tens: a resample's median is 0 with fewer than ten tens (about 41% of
        # resamples), 5 with exactly ten (18%) and 10 with more (41%), so the sorted medians run
        # 0 ... 0, 5 ... 5, 10 ... 10 and indices 250 and 9749 fall in the first and last runs.
        self.assertEqual(analysis.interval([0.0] * 10 + [10.0] * 10), (0.0, 10.0))

    def test_interval_of_a_constant_delta_is_that_delta(self) -> None:
        self.assertEqual(analysis.interval([2.0] * 20), (2.0, 2.0))


class CountTests(unittest.TestCase):
    def test_per_operation_count_is_the_difference_over_k(self) -> None:
        self.assertEqual(analysis.per_operation_count(2000 + 7, 200 + 7, 1000, 100),
                         {"value": 2.0, "unstable": False})

    def test_stable_lists_are_not_flagged(self) -> None:
        result = analysis.per_operation_count([1807, 1807], [207], 1000, 100)
        self.assertFalse(result["unstable"])

    def test_unstable_counts_use_the_maximum(self) -> None:
        result = analysis.per_operation_count([1807, 1810], [207, 205], 1000, 100)
        self.assertTrue(result["unstable"])
        self.assertAlmostEqual(result["value"], (1810 - 205) / 900)


class RuleTests(unittest.TestCase):
    # N of 1,000 ns puts 5% (50 ns) above C1's floor of 3 calls times 5 ns, so these cases judge
    # the relative part of the budget.
    def test_cpu_miss_needs_an_interval_off_zero_and_over_five_percent(self) -> None:
        result = analysis.cpu_rule("C1", 100, cmp(1060, 1000, 60, 40, 80))
        self.assertEqual((result["judged"], result["miss"]), (True, True))

    def test_cpu_interval_crossing_zero_is_not_a_miss(self) -> None:
        self.assertFalse(analysis.cpu_rule("C1", 100, cmp(1100, 1000, 100, -10, 200))["miss"])

    def test_cpu_interval_bound_exactly_at_zero_does_not_exclude_zero(self) -> None:
        self.assertFalse(analysis.cpu_rule("C1", 100, cmp(1100, 1000, 100, 0, 200))["miss"])
        self.assertFalse(analysis.cpu_rule("C1", 100, cmp(800, 1000, -200, -300, 0))["miss"])
        self.assertTrue(analysis.cpu_rule("C1", 100, cmp(1100, 1000, 100, 0.001, 200))["miss"])

    def test_cpu_under_five_percent_is_not_a_miss(self) -> None:
        self.assertFalse(analysis.cpu_rule("C1", 1000, cmp(1040, 1000, 40, 30, 50))["miss"])

    def test_cpu_exactly_five_percent_is_not_a_miss(self) -> None:
        self.assertFalse(analysis.cpu_rule("C1", 100, cmp(1050, 1000, 50, 30, 70))["miss"])

    def test_cpu_budget_is_the_larger_of_five_percent_and_the_floor(self) -> None:
        # C3 makes 2 calls: 10 ns against 5% of 58.4 ns; C1 makes 3: 5% of 1,000 ns against 15 ns
        short = analysis.cpu_rule("C3", 1000, cmp(61.6, 58.4, 3.2, 2.7, 3.6))
        long = analysis.cpu_rule("C1", 1000, cmp(1040, 1000, 40, 30, 50))
        self.assertEqual((short["budget"], long["budget"]), (10.0, 50.0))

    def test_cpu_within_the_floor_and_over_five_percent_passes(self) -> None:
        # C3 at K 1,000 in the 2026-10-21 profile: +3.2 ns [2.7, 3.6], +5.5% of 58.4 ns
        result = analysis.cpu_rule("C3", 1000, cmp(61.6, 58.4, 3.2, 2.7, 3.6))
        self.assertEqual((result["miss"], result["fivePercentMiss"]), (False, True))

    def test_cpu_exactly_at_the_floor_is_not_a_miss(self) -> None:
        result = analysis.cpu_rule("C3", 1000, cmp(68.4, 58.4, 10.0, 9.0, 11.0))
        self.assertEqual((result["miss"], result["fivePercentMiss"]), (False, True))

    def test_cpu_over_the_floor_on_a_short_operation_misses(self) -> None:
        result = analysis.cpu_rule("C3", 100, cmp(65, 50, 15, 14, 16))
        self.assertEqual((result["miss"], result["fivePercentMiss"]), (True, True))

    def test_cpu_long_operation_over_five_percent_and_the_floor_misses(self) -> None:
        result = analysis.cpu_rule("C1", 1000, cmp(1100, 1000, 100, 80, 120))
        self.assertEqual((result["miss"], result["fivePercentMiss"]), (True, True))

    def test_cpu_both_rules_pass_under_five_percent_and_the_floor(self) -> None:
        result = analysis.cpu_rule("C7", 1000, cmp(101.5, 101.4, 0.1, -0.8, 1.2))
        self.assertEqual((result["miss"], result["fivePercentMiss"]), (False, False))

    def test_cpu_five_percent_verdict_is_absent_when_not_judged(self) -> None:
        for result in (analysis.cpu_rule("C1", 10, cmp(200, 100, 100, 90, 110)),
                       analysis.cpu_rule("C1", 100, None)):
            self.assertIsNone(result.get("fivePercentMiss"))

    def test_cpu_k_below_100_is_reported_only(self) -> None:
        result = analysis.cpu_rule("C1", 10, cmp(200, 100, 100, 90, 110))
        self.assertEqual((result["judged"], result["miss"]), (False, None))

    def test_cpu_c10_at_max_count_1_is_reported_only(self) -> None:
        result = analysis.cpu_rule("C10m1", 1000, cmp(200, 100, 100, 90, 110))
        self.assertEqual((result["judged"], result["miss"]), (False, None))

    def test_cpu_c10_is_judged(self) -> None:
        self.assertTrue(analysis.cpu_rule("C10", 100, cmp(200, 100, 100, 90, 110))["miss"])

    def test_cpu_r_faster_is_not_a_miss(self) -> None:
        self.assertFalse(analysis.cpu_rule("C1", 100, cmp(50, 100, -50, -60, -40))["miss"])

    def test_cpu_not_run_is_judged_without_a_verdict(self) -> None:
        result = analysis.cpu_rule("C1", 100, None)
        self.assertEqual((result["judged"], result["miss"], result["reason"]),
                         (True, None, "not run"))

    def test_gpu_misses_over_one_percent_on_the_frame(self) -> None:
        self.assertTrue(analysis.gpu_rule("frame", cmp(102, 100, 2, 1, 3))["miss"])

    def test_gpu_at_one_percent_or_crossing_zero_is_not_a_miss(self) -> None:
        self.assertFalse(analysis.gpu_rule("frame", cmp(101, 100, 1, 0.5, 2))["miss"])
        self.assertFalse(analysis.gpu_rule("frame", cmp(105, 100, 5, -1, 9))["miss"])

    def test_gpu_other_rows_are_reported_only(self) -> None:
        for name in ("G1", "G2", "C1"):
            result = analysis.gpu_rule(name, cmp(200, 100, 100, 90, 110))
            self.assertEqual((result["judged"], result["miss"]), (False, None))

    def test_counts_at_the_boundary(self) -> None:
        n = {"value": 2.0, "unstable": False}
        self.assertFalse(analysis.count_rule("C1", {"value": 5.0, "unstable": False}, n)["miss"])
        self.assertTrue(analysis.count_rule("C1", {"value": 6.0, "unstable": False}, n)["miss"])

    def test_c10_counts_are_not_judged(self) -> None:
        n = {"value": 1.0, "unstable": False}
        result = analysis.count_rule("C10", {"value": 99.0, "unstable": False}, n)
        self.assertEqual((result["judged"], result["miss"]), (False, None))

    def test_unstable_r_counts_are_judged_on_the_maximum(self) -> None:
        n = {"value": 2.0, "unstable": False}
        # (4601 - 0) / 900 = 5.11 is above the registered 2 plus c of 3
        unstable = analysis.per_operation_count([4600, 4601], [0], 1000, 100)
        self.assertTrue(analysis.count_rule("C1", unstable, n)["miss"])
        within = analysis.per_operation_count([4500, 4500], [0], 1000, 100)
        self.assertFalse(analysis.count_rule("C1", within, n)["miss"])

    def test_the_limit_is_the_registered_column_plus_c(self) -> None:
        n = {"value": 2.0, "unstable": False}
        result = analysis.count_rule("C1", {"value": 1.0, "unstable": False}, n)
        self.assertEqual(result["limit"], 5)

    def test_c8_limit_is_its_twelve_native_calls_plus_seven(self) -> None:
        n = {"value": 12.0, "unstable": False}
        within = analysis.count_rule("C8", {"value": 19.0, "unstable": False}, n)
        self.assertEqual((within["limit"], within["miss"]), (19, False))
        self.assertTrue(analysis.count_rule("C8", {"value": 20.0, "unstable": False}, n)["miss"])

    def test_a_native_count_off_the_registered_column_is_not_run(self) -> None:
        r = {"value": 3.0, "unstable": False}
        for n in ({"value": 3.0, "unstable": False}, {"value": 1.0, "unstable": False},
                  {"value": 2.0, "unstable": True}):
            result = analysis.count_rule("C1", r, n)
            self.assertEqual((result["judged"], result["miss"]), (True, None))

    def test_allocations_equal_pass_and_one_more_misses(self) -> None:
        self.assertFalse(analysis.allocation_rule(cmp(3, 3, 0, 0, 0))["miss"])
        self.assertTrue(analysis.allocation_rule(cmp(4, 3, 1, 1, 1))["miss"])

    def test_pipeline_after_warmup_misses(self) -> None:
        self.assertFalse(analysis.pipeline_rule([0, 0])["miss"])
        self.assertTrue(analysis.pipeline_rule([0, 1])["miss"])
        self.assertIsNone(analysis.pipeline_rule(None)["miss"])


def configuration(id_: str, workload: str, **params) -> dict:
    return {"id": id_, "workload": workload, "params": params, "arms": ["R", "N"]}


def processes_of(r: dict, n: dict, count: int = 20) -> dict:
    return {p: {"arms": {"R": r, "N": n}} for p in range(count)}


class SummaryTests(unittest.TestCase):
    def cpu_configs(self):
        return [configuration("C1-k100", "C1", K=100), configuration("C1-k1000", "C1", K=1000),
                configuration("C1-k10", "C1", K=10)]

    def good_processes(self, r_cpu_per_op: float, r_calls=(207, 2007)):
        def proc(cpu, k, calls_at):
            return processes_of(
                arm(cpu=[cpu * k] * 3, allocations=[4, 4, 4], calls=[calls_at] * 3,
                    cpu_allocations=[1, 1, 1], counters=counters(7, 7)),
                arm(cpu=[100.0 * k] * 3, allocations=[4, 4, 4], calls=[2 * k + 7] * 3))
        return {"C1-k10": proc(r_cpu_per_op, 10, 27),
                "C1-k100": proc(r_cpu_per_op, 100, r_calls[0]),
                "C1-k1000": proc(r_cpu_per_op, 1000, r_calls[1])}

    def run_summary(self, processes, notruns=None, passed=True):
        return analysis.summary(self.cpu_configs(), processes, notruns or {},
                                {"C1": {"passed": passed, "failures": []}})

    def test_within_budget(self) -> None:
        result = self.run_summary(self.good_processes(100.0))
        self.assertEqual(result["verdict"], "within budget")
        self.assertEqual(result["decisions"], {"misses": [], "notRun": []})

    def test_cpu_miss_is_listed_and_k10_is_not_judged(self) -> None:
        result = self.run_summary(self.good_processes(120.0))
        self.assertEqual(result["verdict"], "budget missed")
        subjects = [(m["subject"], m["rule"]) for m in result["decisions"]["misses"]]
        self.assertEqual(subjects, [("C1-k100", "cpu"), ("C1-k1000", "cpu")])

    def test_count_miss_names_the_workload(self) -> None:
        # R makes (10107 - 207) / 900 = 11 calls per repetition; N's registered 2 plus c of 3 is 5
        result = self.run_summary(self.good_processes(100.0, r_calls=(207, 2007 + 9 * 900)))
        self.assertEqual(result["workloads"]["C1"]["R"]["value"], 11.0)
        self.assertIn(("C1", "counts"),
                      [(m["subject"], m["rule"]) for m in result["decisions"]["misses"]])

    def test_a_native_arm_off_its_column_is_incomplete_not_a_pass(self) -> None:
        processes = self.good_processes(100.0)
        for id_, k in (("C1-k100", 100), ("C1-k1000", 1000)):
            for process in processes[id_].values():
                process["arms"]["N"]["nativeCalls"] = [3 * k + 7] * 3
        result = self.run_summary(processes)
        self.assertEqual(result["verdict"], "incomplete")
        self.assertIn(("C1", "counts"),
                      [(m["subject"], m["rule"]) for m in result["decisions"]["notRun"]])

    def test_a_pipeline_after_warmup_is_a_miss(self) -> None:
        processes = self.good_processes(100.0)
        processes["C1-k100"][3]["arms"]["R"]["counters"] = counters(7, 8)
        result = self.run_summary(processes)
        self.assertIn(("C1-k100", "pipelines"),
                      [(m["subject"], m["rule"]) for m in result["decisions"]["misses"]])

    def test_a_configuration_that_did_not_run_is_never_a_pass(self) -> None:
        processes = self.good_processes(100.0)
        del processes["C1-k100"]
        result = self.run_summary(processes)
        self.assertEqual(result["verdict"], "incomplete")
        self.assertIn(("C1-k100", "cpu"),
                      [(m["subject"], m["rule"]) for m in result["decisions"]["notRun"]])

    def test_a_partial_set_of_processes_did_not_run(self) -> None:
        processes = self.good_processes(100.0)
        processes["C1-k100"] = {p: v for p, v in processes["C1-k100"].items() if p < 19}
        entry = {e["id"]: e for e in self.run_summary(processes)["configurations"]}["C1-k100"]
        self.assertFalse(entry["ran"])

    def test_notrun_reason_is_carried(self) -> None:
        result = self.run_summary(self.good_processes(100.0),
                                  notruns={"C1-k100": "thermal state serious"})
        entry = {e["id"]: e for e in result["configurations"]}["C1-k100"]
        self.assertEqual(entry["reason"], "thermal state serious")

    def test_failed_correctness_is_not_timed_and_not_passed(self) -> None:
        result = self.run_summary(self.good_processes(100.0), passed=False)
        self.assertEqual(result["verdict"], "incomplete")

    def test_gpu_miss_carries_the_texture_indexing_reminder(self) -> None:
        frame = [configuration("frame", "frame")]
        r = arm(gpu=[103.0] * 3, allocations=[1], counters=counters(1, 1))
        processes = {"frame": processes_of(r, arm(gpu=[100.0] * 3, allocations=[1]))}
        result = analysis.summary(frame, processes, {}, {"frame": {"passed": True}})
        self.assertIn("textureIndexing", result)
        self.assertIn("#30", result["textureIndexing"])

    def test_within_the_floor_is_not_a_miss_and_keeps_the_five_percent_verdict(self) -> None:
        # R 112 against N 100 ns a repetition of C1: 12 ns is under 15 ns, over 5 ns
        result = self.run_summary(self.good_processes(112.0))
        self.assertEqual(result["verdict"], "within budget")
        entry = {e["id"]: e for e in result["configurations"]}["C1-k1000"]
        self.assertEqual((entry["rules"]["cpu"]["miss"], entry["rules"]["cpu"]["fivePercentMiss"]),
                         (False, True))

    def test_report_shows_the_five_percent_verdict_beside_the_rule(self) -> None:
        text = analysis.format_report(self.run_summary(self.good_processes(112.0)))
        self.assertIn("5% rule", text)
        row = next(line for line in text.splitlines() if line.startswith("C1-k1000"))
        self.assertEqual(row.split()[1:3], ["pass", "MISS"])

    def test_report_names_every_rule(self) -> None:
        text = analysis.format_report(self.run_summary(self.good_processes(120.0)))
        for word in ("verdict: budget missed", "CPU per operation", "Native calls", "Allocations",
                     "Pipelines", "MISS"):
            self.assertIn(word, text)


if __name__ == "__main__":
    unittest.main()
