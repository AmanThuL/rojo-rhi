"""Integration tests of the actual xmake rule; mock outputs prove invocation and caching only."""
from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parents[2]
GUARD = r"""{ if pgrep -x RojoRHIBench >/dev/null || pgrep -x FrameDataBench >/dev/null || pgrep -f '^[^ ]*[Pp]ython[0-9.]* [^ ]*(bench\.py timing|_paired\.py)' >/dev/null || pgrep -f '^[^ ]*/App .*--measure' >/dev/null || [ -e /tmp/rojorhi-gpu-measure.lock ]; then echo BUSY; false; else echo FREE; fi; }"""


class RuleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("xmake"):
            raise RuntimeError("actual-rule fixtures require xmake; install it before running this suite")
        if not shutil.which("rtk") and os.environ.get("CI") != "true":
            raise RuntimeError("local actual-rule fixtures require rtk")

    def setUp(self):
        evidence = os.environ.get("SLANG_RULE_EVIDENCE")
        if evidence:
            self.work = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=evidence))
        else:
            self.temporary = tempfile.TemporaryDirectory(prefix="rojorhi-slang-")
            self.addCleanup(self.temporary.cleanup)
            self.work = Path(self.temporary.name)
        self.work = self.work.resolve()
        self.bin = self.work / "bin"
        self.bin.mkdir()
        for name in ("slangc", "xcrun"):
            shutil.copyfile(Path(__file__).with_name("mock_tools.py"), self.bin / name)
            (self.bin / name).chmod(0o755)
        self.log = self.work / "invocations.jsonl"
        self.control = self.work / "control.json"
        self.control.write_text("{}")
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        MOCK_LOG=str(self.log), MOCK_CONTROL=str(self.control))
        self.out = self.work / "out/Shaders"
        for name, entries in (("Ordinary", ["ordinaryMain"]), ("Selected", ["firstMain", "secondMain"])):
            (self.work / (name + ".slang")).write_text("\n".join(
                '[shader("compute")] [numthreads(8, 4, 2)] void ' + entry + '() {}' for entry in entries))
        (self.work / "LayoutProbe.slang").write_text("// layout-only probe\n")
        (self.work / "selection.lua").write_text('target("Fixture")\n')
        quote = lambda p: json.dumps(str(p))
        (self.work / "xmake.lua").write_text(
            'set_project("SlangRuleFixture")\nset_version("0.0.1")\n'
            'option("rojorhi_backend")\n set_default("null")\n set_showmenu(true)\noption_end()\n'
            'includes(' + quote(ROOT / "xmake/slang.lua") + ')\n'
            'target("Fixture")\n set_kind("phony")\n set_targetdir("out")\n add_rules("rojorhi.slang")\n'
            ' add_files("Ordinary.slang", "Selected.slang", "LayoutProbe.slang")\n'
            ' set_values("slang.layout_probe", ' + quote(self.work / "LayoutProbe.slang") + ')\n'
            ' set_values("slang.slangc", ' + quote(self.bin / "slangc") + ')\n'
            ' set_values("slang.moduledir", ' + quote(ROOT / "Shaders/RojoRHI") + ')\n'
            ' set_values("slang.embed", "FixtureShaders.h")\n'
            ' on_load(function (target)\n'
            '  local denied = os.getenv("MOCK_DENY_REMOVE")\n'
            '  if denied then\n'
            '   local original_rm = os.tryrm\n'
            '   os.tryrm = function (file, opt)\n'
            '    if path.absolute(file) == denied then\n'
            '     io.writefile("remove-injection.log", denied)\n'
            '     return false, "fixture filesystem removal denied"\n'
            '    end\n'
            '    return original_rm(file, opt)\n'
            '   end\n'
            '  end\n'
            ' end)\n'
            'target_end()\nincludes("selection.lua")\n')
        self.sequence = 0
        self.configure()

    def command(self, args, success=True):
        command = shlex.join(["xmake", args[0], "-P", ".", *args[1:]])
        # RTK is a local execution policy; hosted CI does not install it.
        wrapped = ["sh", "-c", GUARD + " && " + command]
        if shutil.which("rtk"):
            wrapped = ["rtk", "proxy", *wrapped]
        while True:
            result = subprocess.run(wrapped, cwd=self.work, env=self.env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if not result.stdout.startswith("BUSY"):
                break
            with (self.work / "busy.log").open("a") as stream:
                stream.write(command + "\n" + result.stdout)
            time.sleep(30)
        self.sequence += 1
        (self.work / f"command-{self.sequence:02d}.log").write_text(command + "\n" + result.stdout)
        (self.work / f"command-{self.sequence:02d}-inputs.json").write_text(json.dumps({
            "selection": (self.work / "selection.lua").read_text(), "control": self.control.read_text(),
            "ruleSHA256": hashlib.sha256((ROOT / "xmake/slang.lua").read_bytes()).hexdigest(),
            "mockSHA256": hashlib.sha256((self.bin / "slangc").read_bytes()).hexdigest(),
        }, indent=2) + "\n")
        self.assertEqual(result.returncode == 0, success, result.stdout)
        return result.stdout

    def configure(self, backend="null"):
        self.command(["f", "-m", "debug", "-y", "--rojorhi_backend=" + backend])

    def select(self, *names, raw=None):
        values = raw if raw is not None else ", ".join(json.dumps(name) for name in names)
        setting = ' set_values("slang.precise_files", ' + values + ')\n' if values else ""
        (self.work / "selection.lua").write_text('target("Fixture")\n' + setting)
        self.configure()

    def build(self, success=True):
        if self.log.exists():
            self.log.unlink()
        output = self.command(["build", "Fixture"], success)
        self.calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        (self.work / f"command-{self.sequence:02d}-invocations.json").write_text(json.dumps(self.calls, indent=2) + "\n")
        (self.work / f"command-{self.sequence:02d}-outputs.json").write_text(json.dumps(self.snapshot(), indent=2) + "\n")
        if os.environ.get("SLANG_RULE_EVIDENCE") and self.out.exists():
            shutil.copytree(self.out, self.work / f"command-{self.sequence:02d}-artifacts",
                            copy_function=shutil.copyfile)
        return output

    def source_calls(self, name):
        return [c for c in self.calls if c["tool"] == "slangc" and Path(c["argv"][0]).stem == name]

    def snapshot(self, name=None):
        directories = [self.out] if name else [self.work / "out", self.work / "build/rojorhi"]
        return {str(p.relative_to(self.work)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                for directory in directories for p in directory.rglob("*")
                if p.is_file() and (not name or p.name.startswith(name + "."))}

    def input_snapshot(self):
        files = list(self.work.glob("*.slang")) + [self.bin / "slangc", ROOT / "xmake/slang.lua"]
        files += list((ROOT / "Shaders/RojoRHI").rglob("*.slang"))
        return {str(p): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns) for p in files}

    def assert_policy(self, name, precise):
        calls = self.source_calls(name)
        self.assertEqual(len(calls), 5 if name == "Selected" else 4)
        self.assertEqual(sorted(c["argv"][c["argv"].index("-target") + 1] for c in calls),
                         sorted(["metal", "spirv", "hlsl", "dxil", "dxil"] if name == "Selected"
                                else ["metal", "spirv", "hlsl", "dxil"]))
        for call in calls:
            args = call["argv"]
            self.assertEqual(args.count("-fp-mode"), int(precise), args)
            if precise:
                self.assertEqual(args[args.index("-fp-mode") + 1], "precise")
            self.assertEqual(args.count("-D"), 1)
            self.assertIn("-I", args)
            stage = args[args.index("-target") + 1]
            downstream = precise and stage == "dxil"
            self.assertEqual(args.count("-Xdxc"), int(downstream), args)
            self.assertEqual(args.count("-Gis"), int(downstream), args)
            if downstream:
                self.assertEqual(args[args.index("-Xdxc") + 1], "-Gis")
            if stage == "spirv":
                self.assertIn("-fvk-use-scalar-layout", args)
                self.assertIn("-fvk-use-entrypoint-name", args)
            if stage in ("hlsl", "dxil"):
                self.assertEqual(args[args.index("-profile") + 1], "sm_6_6")
        if sys_platform_metal():
            metals = [c["argv"] for c in self.calls if c["tool"] == "xcrun" and "-o" in c["argv"]
                      and Path(c["argv"][-1]).stem == name]
            self.assertEqual(len(metals), 1)
            for flag in ("-fno-fast-math", "-ffp-contract=off"):
                self.assertEqual(metals[0].count(flag), int(precise))
        for ext in ("metal", "metal.json", "spv", "hlsl", "d3d12lib"):
            self.assertEqual(b"-fp-mode" in (self.out / f"{name}.{ext}").read_bytes(), precise)
        self.assertTrue((self.out / (name + ".compile-policy")).is_file())
        policy = json.loads((self.out / (name + ".compile-policy")).read_text())
        if precise:
            self.assertIn(["-Xdxc", "-Gis"], policy)
        for entry in (["firstMain", "secondMain"] if name == "Selected" else [None]):
            suffix = ("." + entry if entry else "") + ".dxil"
            self.assertEqual(b"-Gis" in (self.out / (name + suffix)).read_bytes(), precise)

    def assert_packages(self):
        for extension in (["metal4lib"] if sys_platform_metal() else []) + ["d3d12lib"]:
            data = (self.out / ("Selected." + extension)).read_bytes()
            self.assertIn(b"abi 2\nentry firstMain compute 8 4 2", data)
            self.assertIn(b"entry secondMain compute 8 4 2", data)
        self.assertEqual((self.out / "Selected.nulllib").read_text(),
                         "rojorhi.null.shaderlibrary\nabi 2\nentry firstMain compute\nentry secondMain compute\n")
        self.assertTrue((self.work / "build/rojorhi/Fixture/FixtureShaders.h").is_file())

    def test_selected_all_target_flags(self):
        self.select("Selected")
        self.build()
        self.assert_policy("Selected", True)
        self.assert_policy("Ordinary", False)
        self.assert_packages()

    def test_default_selection_transitions_and_noop(self):
        self.build()
        self.assert_policy("Ordinary", False)
        self.assert_policy("Selected", False)
        ordinary = self.snapshot("Ordinary")
        inputs = self.input_snapshot()
        for precise in (True, False):
            self.select(*(["Selected"] if precise else []))
            self.build()
            self.assert_policy("Selected", precise)
            self.assertEqual(self.source_calls("Ordinary"), [])
            self.assertEqual(self.snapshot("Ordinary"), ordinary)
            self.assertEqual(self.input_snapshot(), inputs)
            self.assert_packages()
            snapshot = self.snapshot()
            self.build()
            self.assertEqual(self.source_calls("Ordinary") + self.source_calls("Selected"), [])
            self.assertEqual(self.snapshot(), snapshot)

    def test_invalid_selection_precedes_probes_and_mutation(self):
        self.build()
        snapshot = self.snapshot()
        for raw in ('"Unknown"', '"Selected.slang"', '"dir/Selected"', json.dumps(r"dir\Selected"),
                    '"selected"', '"Selected", "Selected"', '"Selected", "selected"', '42', 'false',
                    '"LayoutProbe"', '"RojoRHI"'):
            with self.subTest(raw=raw):
                self.select(raw=raw)
                output = self.build(False)
                self.assertIn("slang.precise_files", output)
                self.assertIn("Fixture", output)
                self.assertEqual(self.calls, [])
                self.assertEqual(self.snapshot(), snapshot)

    def test_reordered_names_leave_effective_policy_unchanged(self):
        self.select("Selected", "Ordinary")
        self.build()
        snapshot = self.snapshot()
        self.select("Ordinary", "Selected")
        self.build()
        self.assertEqual(self.source_calls("Ordinary") + self.source_calls("Selected"), [])
        self.assertEqual(self.snapshot(), snapshot)

    def test_failure_retry_and_policy_reversion(self):
        for revert in (False, True):
            with self.subTest(revert=revert):
                self.select()
                self.control.write_text("{}")
                self.build()
                self.select("Selected")
                self.control.write_text(json.dumps({"fail": "spirv"}))
                self.assertIn("mock selected spirv diagnostic", self.build(False))
                self.assertEqual(len(self.source_calls("Selected")), 2)
                self.assertFalse((self.out / "Selected.compile-policy").exists())
                if revert:
                    self.select()
                self.control.write_text("{}")
                self.build()
                self.assert_policy("Selected", not revert)
                self.assert_packages()

    @contextmanager
    def undeletable_product(self, product, inject=False):
        """Deny removal at the filesystem boundary; never substitute rule or policy logic."""
        initial = product.read_bytes()
        flags = getattr(product.stat(), "st_flags", 0)
        native = False
        if not inject and hasattr(os, "chflags") and hasattr(stat, "UF_IMMUTABLE"):
            try:
                os.chflags(product, flags | stat.UF_IMMUTABLE)
                native = True
            except OSError:
                pass
        if not native:
            self.env["MOCK_DENY_REMOVE"] = str(product.absolute())
        receipt = {"product": str(product), "method": "uchg" if native else "os.tryrm injection",
                   "before": self.snapshot("Selected"), "originalFlags": flags}
        try:
            yield
        finally:
            # Clear only this fixture's flag, including when an assertion or the actual build fails.
            if native:
                os.chflags(product, flags)
            self.env.pop("MOCK_DENY_REMOVE", None)
            receipt["after"] = self.snapshot("Selected")
            receipt["blockedBytesUnchanged"] = product.read_bytes() == initial
            receipt["finalFlags"] = getattr(product.stat(), "st_flags", 0)
            stamp = self.out / "Selected.compile-policy"
            receipt["successRecord"] = stamp.read_text() if stamp.exists() else None
            (self.work / f"removal-{self.sequence:02d}.json").write_text(json.dumps(receipt, indent=2) + "\n")
            if not native:
                self.assertTrue((self.work / "remove-injection.log").exists(), "filesystem injection did not run")

    def test_undeletable_product_retry_and_policy_reversion(self):
        self.build()
        ordinary = self.snapshot("Ordinary")
        for revert in (False, True):
            with self.subTest(revert=revert):
                self.select()
                self.build()
                product = self.out / "Selected.hlsl"
                selected = self.snapshot("Selected")
                inputs = self.input_snapshot()
                self.select("Selected")
                with self.undeletable_product(product):
                    output = self.build(False)
                    self.assertIn("cannot remove", output)
                    self.assertIn("Selected.hlsl", output)
                    self.assertFalse((self.out / "Selected.compile-policy").exists())
                    self.assertEqual(self.source_calls("Selected"), [])
                    self.assertEqual(self.snapshot("Ordinary"), ordinary)
                    self.assertEqual(self.input_snapshot(), inputs)
                    self.assertEqual(self.snapshot("Selected")["out/Shaders/Selected.hlsl"],
                                     selected["out/Shaders/Selected.hlsl"])
                if revert:
                    self.select()
                self.build()
                self.assert_policy("Selected", not revert)
                self.assert_packages()
                self.assertEqual(self.snapshot("Ordinary"), ordinary)

    def test_failed_removal_checks_all_product_families(self):
        self.build()
        ordinary = self.snapshot("Ordinary")
        extensions = ["metal", "metal.json", "spv", "hlsl", "nulllib", "dxil",
                      "firstMain.dxil", "secondMain.dxil", "d3d12lib"]
        if sys_platform_metal():
            extensions += ["metallib", "metal4lib"]
        for extension in extensions:
            with self.subTest(extension=extension):
                self.select()
                self.build()
                product = self.out / ("Selected." + extension)
                if extension == "dxil":
                    # A retained single-entry product from an earlier inventory must be cleared too.
                    product.write_bytes((self.out / "Selected.firstMain.dxil").read_bytes())
                self.select("Selected")
                # Exercise the portable fallback too, even on a host supporting native immutable flags.
                with self.undeletable_product(product, inject=True):
                    output = self.build(False)
                    self.assertIn("cannot remove", output)
                    self.assertIn(product.name, output)
                    self.assertFalse((self.out / "Selected.compile-policy").exists())
                    self.assertEqual(self.source_calls("Selected"), [])
                self.build()
                self.assert_policy("Selected", True)
                self.assert_packages()
                self.assertEqual(self.snapshot("Ordinary"), ordinary)

    def test_late_stage_failure_and_missing_outputs(self):
        self.build()
        for stage in (["metallib"] if sys_platform_metal() else []) + ["dxil"]:
            with self.subTest(stage=stage):
                self.select("Selected")
                # The prior iteration may already be precise; force a rebuild by removing one output.
                (self.out / "Selected.spv").unlink(missing_ok=True)
                self.control.write_text(json.dumps({"fail": stage}))
                self.assertIn("mock selected " + stage + " diagnostic", self.build(False))
                self.assertFalse((self.out / "Selected.compile-policy").exists())
                self.control.write_text("{}")
                self.build()
                self.assert_policy("Selected", True)
                self.assert_packages()
        for ext in (["metallib", "metal4lib"] if sys_platform_metal() else []) + [
                "metal", "metal.json", "spv", "hlsl", "nulllib", "firstMain.dxil", "d3d12lib"]:
            (self.out / ("Selected." + ext)).unlink()
            self.build()
            self.assert_packages()
            self.assertTrue((self.out / ("Selected." + ext)).is_file())

    def test_d3d12_requires_successful_dxil_probe(self):
        self.configure("d3d12")
        self.control.write_text(json.dumps({"no_dxil": True}))
        self.assertIn("a d3d12 build requires DXIL", self.build(False))
        self.assertEqual(self.source_calls("Ordinary") + self.source_calls("Selected"), [])
        self.assertFalse((self.out / "Selected.compile-policy").exists())

    def test_optional_outputs_change_effective_policy(self):
        self.select("Selected")
        self.build()
        self.control.write_text(json.dumps({"no_dxil": True, "no_metal": True}))
        output = self.build()
        self.assertIn("DXIL skipped", output)
        self.assertEqual(len(self.source_calls("Selected")), 3)
        self.assertFalse((self.out / "Selected.d3d12lib").exists())
        if sys_platform_metal():
            self.assertFalse((self.out / "Selected.metal4lib").exists())
        self.control.write_text("{}")
        self.build()
        self.assert_policy("Selected", True)
        self.assert_packages()


def sys_platform_metal():
    import sys
    return sys.platform == "darwin"


if __name__ == "__main__":
    unittest.main()
