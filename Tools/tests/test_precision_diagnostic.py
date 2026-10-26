from __future__ import annotations

import hashlib
import json
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


if __name__ == "__main__":
    unittest.main()
