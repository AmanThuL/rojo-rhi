from __future__ import annotations

import hashlib
import json
import copy
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from Tools import precision_diagnostic as diagnostic


def result_xml(successes=8, failures=0, name=diagnostic.CASE, skipped=0):
    return (
        f'<Catch2TestRun><TestCase name="{name}"><OverallResult success="{str(not failures).lower()}" skips="{skipped}"/>'
        '</TestCase>'
        f'<OverallResults successes="{successes}" failures="{failures}" expectedFailures="0" skips="{skipped}"/>'
        f'<OverallResultsCases successes="{int(not failures)}" failures="{int(bool(failures))}" '
        f'expectedFailures="0" skips="{skipped}"/></Catch2TestRun>'
    ).encode()


class PrecisionResultTests(unittest.TestCase):
    def test_exact_case_eight_assertions_and_zero_exit_pass(self):
        self.assertEqual(
            diagnostic.validate_result(result_xml(), diagnostic.CASE + "\n", {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 0}),
            {"passed": True, "assertions": 8, "successes": 8, "failures": 0},
        )

    def test_numerical_failure_is_retained_as_failure(self):
        self.assertEqual(
            diagnostic.validate_result(result_xml(7, 1), diagnostic.CASE, {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 42}),
            {"passed": False, "assertions": 8, "successes": 7, "failures": 1},
        )

    def test_missing_extra_or_changed_case_cannot_pass(self):
        for inventory in ("", diagnostic.CASE + "\nanother case", "another case"):
            with self.subTest(inventory=inventory), self.assertRaises(ValueError):
                diagnostic.validate_result(result_xml(), inventory, {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 0})
        with self.assertRaises(ValueError):
            diagnostic.validate_result(result_xml(name="another case"), diagnostic.CASE, {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 0})

    def test_incomplete_or_skipped_run_cannot_pass(self):
        for xml in (b"", b"<Catch2TestRun/>", result_xml(7), result_xml(skipped=1)):
            with self.subTest(xml=xml), self.assertRaises(ValueError):
                diagnostic.validate_result(xml, diagnostic.CASE, {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 0})

    def test_missing_nonzero_or_mistyped_exit_cannot_pass(self):
        for invocation in ({}, {"inventoryExit": 1, "testExit": 0}, {"inventoryExit": 0, "testExit": None},
                           {"inventoryExit": 0, "testExit": True}, {"inventoryExit": 0, "testExit": 1}):
            with self.subTest(invocation=invocation), self.assertRaises(ValueError):
                diagnostic.validate_result(result_xml(), diagnostic.CASE, dict(invocation, case=diagnostic.CASE))

    def test_invocation_case_and_duplicate_totals_are_rejected(self):
        with self.assertRaises(ValueError):
            diagnostic.validate_result(result_xml(), diagnostic.CASE, {"case": "another case", "inventoryExit": 0, "testExit": 0})
        duplicate = result_xml().replace(b"</Catch2TestRun>", b'<OverallResults successes="8" failures="0" expectedFailures="0" skips="0"/></Catch2TestRun>')
        with self.assertRaises(ValueError):
            diagnostic.validate_result(duplicate, diagnostic.CASE, {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 0})


class PrecisionInputTests(unittest.TestCase):
    def test_only_exact_original_payload_in_one_entry_is_admitted(self):
        dxil = bytearray(3420)
        dxil[:4] = b"DXBC"
        dxil[4:20] = b"1" * 16
        struct.pack_into("<I", dxil, 24, len(dxil))
        data = bytes(dxil)
        container = b"rojorhi.d3d12.shaderlibrary\nabi 4\nentry preciseMain compute 1 1 1 3420\ndxil 3420\n" + data
        with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(data).hexdigest(), create=True):
            self.assertEqual(diagnostic.verify_library(container, data, 4), hashlib.sha256(data).hexdigest())
            for changed in (container + b"x", container.replace(b"preciseMain", b"anotherMain"), container.replace(b"abi 4", b"abi 3")):
                with self.subTest(changed=changed[:80]), self.assertRaises(ValueError):
                    diagnostic.verify_library(changed, data, 4)
        with self.assertRaises(ValueError):
            diagnostic.verify_library(container, data, 4)

    def test_missing_changed_or_extra_input_is_rejected(self):
        before = {"files": {"a": {"size": 3, "sha256": "a"}}, "versions": {"a": "1.0"}}
        self.assertEqual(diagnostic.compare_closure(before, before), [])
        for after in ({"files": {}, "versions": before["versions"]},
                      {"files": {"a": {"size": 3, "sha256": "b"}}, "versions": before["versions"]},
                      {"files": dict(before["files"], b={"size": 1, "sha256": "b"}), "versions": before["versions"]},
                      {"files": before["files"], "versions": {"a": "2.0"}}):
            with self.subTest(after=after):
                self.assertTrue(diagnostic.compare_closure(before, after))

    def test_requires_debug_gbv_and_each_setting_once(self):
        text = '\n'.join(f'{key} = "{value}",' for key, value in
                         {"mode": "debug", "rojorhi_backend": "d3d12", "rojorhi_d3d12_adapter": "warp", "rojorhi_d3d12_debug": "gbv"}.items())
        self.assertEqual(diagnostic.verify_config(text)["rojorhi_d3d12_debug"], "gbv")
        for changed in (text.replace('"gbv"', '"layer"'), text.replace('"debug"', '"release"'),
                        text + '\nmode = "debug",', ""):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                diagnostic.verify_config(changed)

    def test_missing_pdb_and_missing_redirected_output_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(ValueError):
                diagnostic.collect_files(root)
            with self.assertRaises(ValueError):
                diagnostic.after(root, root)

    def test_prior_preparation_error_stays_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "before-error.json").write_text('{"passed": false}')
            with self.assertRaises(ValueError):
                diagnostic.checked_closure(root, root)


class PrecisionSnapshotTests(unittest.TestCase):
    def fixture(self, root):
        for name in diagnostic.SOURCES:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"source")
        module = root / "Shaders/RojoRHI/RojoRHI.slang"
        module.write_text("public static const uint ROJORHI_SHADER_ABI_VERSION = 4;")
        directory = root / diagnostic.TEST_DIR
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "RojoRHITests.exe").write_bytes(b"test executable")
        (directory / "RojoRHITests.pdb").write_bytes(b"test PDB")
        for name, source in diagnostic.DLLS.items():
            for relative in (diagnostic.TEST_DIR / name, Path(source)):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"runtime")
        shaders = directory / "Shaders"
        shaders.mkdir()
        (shaders / "Standard.d3d12lib").write_bytes(b"standard")
        dxil = bytearray(3420)
        dxil[:4] = b"DXBC"
        dxil[4:20] = b"1" * 16
        struct.pack_into("<I", dxil, 24, len(dxil))
        (shaders / "AbiPrecise.dxil").write_bytes(dxil)
        (shaders / "AbiPrecise.d3d12lib").write_bytes(
            b"rojorhi.d3d12.shaderlibrary\nabi 4\nentry preciseMain compute 1 1 1 3420\ndxil 3420\n" + dxil)
        config = root / ".xmake/windows/x64/xmake.conf"
        config.parent.mkdir(parents=True)
        config.write_text('mode = "debug",\nrojorhi_backend = "d3d12",\nrojorhi_d3d12_adapter = "warp",\nrojorhi_d3d12_debug = "gbv",\n')
        return bytes(dxil)

    def prepare(self, root, dxil):
        output = root / diagnostic.OUTPUT
        with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(dxil).hexdigest()), \
             patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()):
            diagnostic.before(root, output)
        before = json.loads((output / "before.json").read_text())
        versions = {name: "1.0.20.0" for name in before["files"] if name.endswith(".dll")}
        (output / "versions-before.json").write_text(json.dumps(versions))
        return output, before, versions

    def test_snapshot_copies_all_bound_bytes_and_a_failed_oracle_stays_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dxil = self.fixture(root)
            output, before, versions = self.prepare(root, dxil)
            for name, identity in before["files"].items():
                self.assertEqual(hashlib.sha256((output / "inputs" / name).read_bytes()).hexdigest(), identity["sha256"])
            (output / "versions-after.json").write_text(json.dumps(versions))
            (output / "inventory.stdout").write_text(diagnostic.CASE + "\n")
            for name in ("inventory.stderr", "test.stdout", "test.stderr"):
                (output / name).write_text("")
            (output / "invocation.json").write_text(json.dumps({"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 42}))
            (output / "result.xml").write_bytes(result_xml(7, 1))
            with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(dxil).hexdigest()), \
                 patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()):
                self.assertEqual(diagnostic.after(root, output), 1)
            self.assertFalse(json.loads((output / "result.json").read_text())["outcome"]["passed"])
            self.assertTrue((output / "after.json").is_file())

    def test_changed_debug_input_is_rejected_and_its_actual_identity_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dxil = self.fixture(root)
            output, _, _ = self.prepare(root, dxil)
            executable = root / diagnostic.TEST_DIR / "RojoRHITests.exe"
            executable.write_bytes(b"a later build")
            with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(dxil).hexdigest()), \
                 patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()), self.assertRaises(ValueError):
                diagnostic.checked_closure(root, output)
            observed = output / "check-closure.json"
            self.assertTrue(observed.is_file())
            self.assertEqual(json.loads(observed.read_text())["files"][(diagnostic.TEST_DIR / "RojoRHITests.exe").as_posix()]["sha256"],
                             hashlib.sha256(b"a later build").hexdigest())

    def test_changed_archived_bytes_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dxil = self.fixture(root)
            output, _, _ = self.prepare(root, dxil)
            (output / "inputs" / diagnostic.TEST_DIR / "RojoRHITests.exe").write_bytes(b"corrupt")
            with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(dxil).hexdigest()), \
                 patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()), self.assertRaises(ValueError):
                diagnostic.checked_closure(root, output)


def mapped_snapshot(executable=r"D:\work\build\windows\x64\debug\rojorhi-test\RojoRHITests.exe", system_folder=None):
    folder = str(Path(executable).parent) if executable.startswith("/") else executable.rsplit("\\", 1)[0]
    separator = "/" if executable.startswith("/") else "\\"
    system_folder = system_folder or r"C:\Windows\System32"
    paths = [executable, folder + separator + "D3D12" + separator + "D3D12Core.dll",
             folder + separator + "d3d10warp.dll", system_folder + separator + "d3d12.dll",
             system_folder + separator + "D3D12Core.dll"]
    modules = [{"path": path, "base": hex((i + 1) * 65536), "size": 4096,
                "pathCharacters": len(path.encode("utf-16-le")) // 2, "pathCapacity": 260} for i, path in enumerate(paths)]
    snapshot = {"complete": True, "errors": [], "attempts": [{"success": True, "error": 0,
                "capacityBytes": 1024, "neededBytes": len(modules) * 8}], "modules": modules}
    return {"schemaVersion": 1, "complete": True, "errors": [], "case": diagnostic.CASE,
            "sourceFile": "Tests/Conformance/PrecisionConformanceTests.cpp", "sourceLine": 75,
            "expression": "resultBits == 0u", "phase": "assertionStarting", "processId": 1234,
            "pointerBytes": 8, "executablePath": executable, "snapshots": [snapshot, copy.deepcopy(snapshot)]}


class PrecisionMappedModulesTests(unittest.TestCase):
    def validate(self, raw):
        return getattr(diagnostic, "validate_mapped_snapshot", lambda *unused: {})(raw, raw["executablePath"])

    def test_all_core_instances_remain_distinct(self):
        raw = mapped_snapshot()
        result = self.validate(raw)
        self.assertEqual(len(result.get("relevantImages", [])), 4)
        self.assertEqual(len([m for m in result.get("relevantImages", []) if m["path"].lower().endswith("d3d12core.dll")]), 2)

    def test_wrong_trigger_and_types_are_rejected(self):
        for key, value in (("case", "another case"), ("sourceLine", 74), ("sourceLine", True),
                           ("sourceFile", "Another.cpp"), ("expression", "resultBits != 0u"),
                           ("phase", "testCaseEnded"), ("processId", 0), ("processId", True), ("pointerBytes", 4)):
            raw = mapped_snapshot()
            raw[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(raw)

    def test_failed_partial_and_unstable_enumerations_are_rejected(self):
        samples = []
        raw = mapped_snapshot(); raw["complete"] = False; raw["errors"] = ["native failure"]; samples.append(raw)
        raw = mapped_snapshot(); raw["snapshots"] = raw["snapshots"][:1]; samples.append(raw)
        raw = mapped_snapshot(); raw["snapshots"][1]["modules"][0]["size"] += 1; samples.append(raw)
        raw = mapped_snapshot(); raw["snapshots"][0]["attempts"][0]["success"] = False; samples.append(raw)
        raw = mapped_snapshot(); raw["snapshots"][0]["attempts"][0]["neededBytes"] = 2048; samples.append(raw)
        for raw in samples:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                self.validate(raw)

    def test_truncated_relative_and_repeated_instance_paths_are_rejected(self):
        for change in ("truncate", "relative", "repeat", "base", "nul"):
            raw = mapped_snapshot()
            for snapshot in raw["snapshots"]:
                if change == "truncate": snapshot["modules"][0]["pathCapacity"] = 1
                if change == "relative": snapshot["modules"][0]["path"] = "relative.dll"
                if change == "repeat": snapshot["modules"][1]["base"] = snapshot["modules"][0]["base"]
                if change == "base": snapshot["modules"][0]["base"] = "0x0"
                if change == "nul": snapshot["modules"][0]["path"] += "\x00"
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate(raw)

    def test_unexpected_or_duplicate_warp_is_observed_but_not_accepted(self):
        for duplicate in (False, True):
            raw = mapped_snapshot()
            for snapshot in raw["snapshots"]:
                entry = copy.deepcopy(snapshot["modules"][2])
                entry.update(path=r"C:\Windows\System32\d3d10warp.dll", base="0x990000")
                entry["pathCharacters"] = len(entry["path"])
                if duplicate: snapshot["modules"].append(entry)
                else: snapshot["modules"][2] = entry
                snapshot["attempts"][-1]["neededBytes"] = len(snapshot["modules"]) * 8
            result = self.validate(raw)
            self.assertEqual(result.get("expectedUniqueWarp"), False)
            self.assertEqual(len([m for m in result.get("relevantImages", []) if m["path"].lower().endswith("d3d10warp.dll")]),
                             2 if duplicate else 1)

    def test_observed_disk_paths_are_hashed_and_versions_are_required(self):
        raw = mapped_snapshot()
        paths = {m["path"] for m in raw["snapshots"][0]["modules"] if m["path"].lower().endswith(".dll")}
        versions = {p: "1.0.20.0" for p in paths}
        function = getattr(diagnostic, "mapped_identity", lambda *args, **kwargs: {})
        result = function(raw, raw["executablePath"], versions, read_bytes=lambda path: b"runtime")
        self.assertEqual(set(result.get("files", {})), paths)
        self.assertEqual(result.get("complete"), True)
        with self.assertRaises(ValueError):
            function(raw, raw["executablePath"], {}, read_bytes=lambda path: b"runtime")

    def test_mistyped_error_and_out_of_range_native_metadata_fail(self):
        for field in ("error-type", "capacity", "process"):
            raw = mapped_snapshot()
            if field == "process": raw["processId"] = 2**40
            for snapshot in raw["snapshots"]:
                if field == "error-type": snapshot["attempts"][0]["error"] = False
                if field == "capacity": snapshot["attempts"][0]["capacityBytes"] = 2**40
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(raw)

    def test_system_core_alone_does_not_confirm_the_expected_private_mapping(self):
        raw = mapped_snapshot()
        for snapshot in raw["snapshots"]:
            del snapshot["modules"][1]
            snapshot["attempts"][0]["neededBytes"] = len(snapshot["modules"]) * 8
        versions = {m["path"]: "1.0.20.0" for m in raw["snapshots"][0]["modules"] if m["path"].endswith(".dll")}
        with patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()):
            result = diagnostic.mapped_identity(raw, raw["executablePath"], versions, read_bytes=lambda path: b"runtime")
        self.assertEqual(result.get("complete"), True)
        self.assertEqual(result.get("expectedPrivateCoreMapped"), False)
        self.assertEqual(result.get("expectedDeployment"), False)


class PrecisionMappedOutcomeTests(unittest.TestCase):
    def prepared(self, root, failed=False, observed=True):
        fixture = PrecisionSnapshotTests()
        dxil = fixture.fixture(root)
        output, _, versions = fixture.prepare(root, dxil)
        (output / "versions-after.json").write_text(json.dumps(versions))
        (output / "inventory.stdout").write_text(diagnostic.CASE + "\n")
        for name in ("inventory.stderr", "test.stdout", "test.stderr"):
            (output / name).write_text("")
        (output / "result.xml").write_bytes(result_xml(7, 1) if failed else result_xml())
        invocation = {"case": diagnostic.CASE, "inventoryExit": 0, "testExit": 42 if failed else 0,
                      "moduleObservationRequested": True,
                      "moduleObservationPath": str((output / "mapped-modules.json").resolve())}
        (output / "invocation.json").write_text(json.dumps(invocation))
        if observed:
            executable = str((root / diagnostic.TEST_DIR / "RojoRHITests.exe").resolve())
            system_folder = root / "external-system"
            system_folder.mkdir()
            for name in ("d3d12.dll", "D3D12Core.dll"):
                (system_folder / name).write_bytes(b"external runtime")
            raw = mapped_snapshot(executable, str(system_folder))
            (output / "mapped-modules.json").write_text(json.dumps(raw))
            (output / "mapped-versions.json").write_text(json.dumps({m["path"]: "1.0.20.0"
                for m in raw["snapshots"][0]["modules"] if m["path"].endswith(".dll")}))
        return output, dxil

    def finish(self, root, output, dxil):
        with patch.object(diagnostic, "DXIL_SHA", hashlib.sha256(dxil).hexdigest()), \
             patch.object(diagnostic, "WARP_SHA", hashlib.sha256(b"runtime").hexdigest()):
            return diagnostic.after(root, output)

    def test_numeric_pass_without_observer_does_not_pass_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, dxil = self.prepared(root, observed=False)
            self.assertEqual(self.finish(root, output, dxil), 1)
            result = json.loads((output / "result.json").read_text())
            self.assertTrue(result["outcome"]["passed"])
            self.assertEqual(result.get("mappedIdentity", {}).get("complete"), False)

    def test_numeric_failure_and_valid_identity_remain_independent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, dxil = self.prepared(root, failed=True)
            self.assertEqual(self.finish(root, output, dxil), 1)
            result = json.loads((output / "result.json").read_text())
            self.assertFalse(result["outcome"]["passed"])
            self.assertEqual(result.get("mappedIdentity", {}).get("expectedDeployment"), True)

    def test_positive_identity_keeps_after_only_system_files_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, dxil = self.prepared(root)
            self.assertEqual(self.finish(root, output, dxil), 0)
            identity = json.loads((output / "result.json").read_text()).get("mappedIdentity", {})
            self.assertEqual(identity.get("complete"), True)
            self.assertEqual(identity.get("expectedDeployment"), True)
            files = identity.get("files", {})
            self.assertEqual({item.get("binding") for item in files.values()}, {"frozen-input", "after-only"})

    def test_partial_write_and_sticky_version_error_preserve_numeric_result(self):
        for error in ("partial", "versions"):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                output, dxil = self.prepared(root)
                if error == "partial": (output / "mapped-modules.json").write_text('{"incomplete":')
                else: (output / "mapped-versions-error.txt").write_text("an earlier metadata failure")
                self.assertEqual(self.finish(root, output, dxil), 1)
                result = json.loads((output / "result.json").read_text())
                self.assertTrue(result["outcome"]["passed"])
                self.assertEqual(result.get("mappedIdentity", {}).get("complete"), False)

    def test_complete_json_with_observer_write_or_close_error_stays_failed(self):
        for marker in ("mapped-image output write failed", "mapped-image output close failed"):
            for failed in (False, True):
                with self.subTest(marker=marker, failed=failed), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    output, dxil = self.prepared(root, failed=failed)
                    raw = (output / "mapped-modules.json").read_bytes()
                    stderr = ("precision mapped-image observation: " + marker + "\n").encode()
                    (output / "test.stderr").write_bytes(stderr)
                    self.assertEqual(self.finish(root, output, dxil), 1)
                    result = json.loads((output / "result.json").read_text())
                    self.assertTrue(result["numericalComplete"])
                    self.assertEqual(result["outcome"], {"passed": not failed, "assertions": 8,
                                     "successes": 7 if failed else 8, "failures": 1 if failed else 0})
                    self.assertFalse(result["mappedIdentity"]["complete"])
                    self.assertFalse(result["mappedIdentity"]["expectedDeployment"])
                    self.assertIn("observer reported failure", result["mappedIdentity"]["errors"][0]["message"])
                    self.assertFalse(result["diagnosticPassed"])
                    self.assertEqual((output / "test.stderr").read_bytes(), stderr)
                    self.assertEqual((output / "mapped-modules.json").read_bytes(), raw)

    def test_observer_error_channel_read_failure_preserves_numeric_result(self):
        for failed in (False, True):
            with self.subTest(failed=failed), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                output, dxil = self.prepared(root, failed=failed)
                stderr = output / "test.stderr"
                raw = (output / "mapped-modules.json").read_bytes()
                read_bytes = Path.read_bytes

                def read(path):
                    if path == stderr:
                        raise OSError("synthetic observer error-channel read failure")
                    return read_bytes(path)

                with patch.object(Path, "read_bytes", read):
                    self.assertEqual(self.finish(root, output, dxil), 1)
                result = json.loads((output / "result.json").read_text())
                self.assertTrue(result["numericalComplete"])
                self.assertEqual(result["outcome"]["passed"], not failed)
                self.assertEqual(result["outcome"]["assertions"], 8)
                self.assertFalse(result["mappedIdentity"]["complete"])
                self.assertFalse(result["mappedIdentity"]["expectedDeployment"])
                self.assertEqual(result["mappedIdentity"]["errors"][0]["errorType"], "OSError")
                self.assertFalse(result["diagnosticPassed"])
                self.assertEqual(stderr.read_bytes(), b"")
                self.assertEqual((output / "mapped-modules.json").read_bytes(), raw)

    def test_unrelated_stderr_does_not_invalidate_complete_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output, dxil = self.prepared(root)
            stderr = b"ordinary diagnostic output\n\xff\n"
            (output / "test.stderr").write_bytes(stderr)
            self.assertEqual(self.finish(root, output, dxil), 0)
            result = json.loads((output / "result.json").read_text())
            self.assertTrue(result["numericalComplete"])
            self.assertTrue(result["mappedIdentity"]["complete"])
            self.assertTrue(result["diagnosticPassed"])
            self.assertEqual((output / "test.stderr").read_bytes(), stderr)


if __name__ == "__main__":
    unittest.main()
