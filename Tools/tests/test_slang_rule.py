"""Runs the rojorhi.slang rule of xmake/slang.lua on a fixture project with a mock compiler, to check which flags
slang.precise_files gives each compile and that a changed selection or a failed build rebuilds the whole file."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("xmake") and sys.platform != "win32", "needs xmake and a POSIX shell for the mocks")
class PreciseFilesTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="rojorhi-slang-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name).resolve()
        bin_dir = self.work / "bin"
        bin_dir.mkdir()
        for tool in ("slangc", "xcrun"):
            shutil.copyfile(Path(__file__).with_name("slang_rule_mock.py"), bin_dir / tool)
            (bin_dir / tool).chmod(0o755)
        self.log = self.work / "calls.jsonl"
        self.env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}", MOCK_LOG=str(self.log))
        self.env.pop("MOCK_FAIL", None)
        (self.work / "Ordinary.slang").write_text('[shader("compute")] [numthreads(1, 1, 1)] void ordinaryMain() {}\n')
        (self.work / "Selected.slang").write_text('[shader("compute")] [numthreads(1, 1, 1)] void firstMain() {}\n'
                                                  '[shader("compute")] [numthreads(1, 1, 1)] void secondMain() {}\n')
        self.out = self.work / "out" / "Shaders"
        self.select()

    def select(self, *names: str) -> None:
        """Writes the fixture project with slang.precise_files set to `names`."""
        quoted = lambda value: json.dumps(str(value))
        precise = f' set_values("slang.precise_files", {", ".join(quoted(n) for n in names)})\n' if names else ""
        (self.work / "xmake.lua").write_text(
            'option("rojorhi_backend")\n set_default("null")\noption_end()\n'
            f'includes({quoted(ROOT / "xmake" / "slang.lua")})\n'
            'target("Fixture")\n set_kind("phony")\n set_targetdir("out")\n add_rules("rojorhi.slang")\n'
            ' add_files("Ordinary.slang", "Selected.slang")\n'
            f' set_values("slang.slangc", {quoted(self.work / "bin" / "slangc")})\n'
            f' set_values("slang.moduledir", {quoted(ROOT / "Shaders" / "RojoRHI")})\n' + precise)

    def build(self, succeeds: bool = True) -> str:
        """Configures and builds the fixture; returns the build output and leaves the compile calls in `self.calls`."""
        self.log.unlink(missing_ok=True)
        for args in (["f", "-y", "-P", "."], ["build", "-P", ".", "Fixture"]):
            result = subprocess.run(["xmake", *args], cwd=self.work, env=self.env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if args[0] == "f":
                self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout)
        lines = self.log.read_text().splitlines() if self.log.exists() else []
        self.calls = [call for call in map(json.loads, lines) if call["argv"][:2] != ["-f", "metal"]
                      and call["argv"] != ["-version"] and "DxilProbe" not in call["argv"][0]]
        return result.stdout

    def compiles(self, stem: str) -> list[list[str]]:
        """The argument lists of every compile of the entry file `stem`."""
        return [call["argv"] for call in self.calls
                if Path(call["argv"][0] if call["tool"] == "slangc" else call["argv"][-1]).name.split(".")[0] == stem]

    def assert_precision(self, stem: str, precise: bool) -> None:
        compiles = self.compiles(stem)
        self.assertTrue(compiles)
        for argv in compiles:
            with self.subTest(argv=argv):
                if "-target" in argv:
                    dxil = argv[argv.index("-target") + 1] == "dxil"
                    self.assertEqual("-fp-mode" in argv, precise)
                    if precise:
                        self.assertEqual(argv[argv.index("-fp-mode") + 1], "precise")
                    self.assertEqual("-Gis" in argv, precise and dxil)
                else:
                    flags = {"-fno-fast-math", "-ffp-contract=off"}
                    self.assertEqual(flags & set(argv), flags if precise else set())

    def test_a_selected_file_compiles_precise_on_every_target(self) -> None:
        self.select("Selected")
        self.build()
        self.assert_precision("Selected", True)
        self.assert_precision("Ordinary", False)
        targets = sorted(argv[argv.index("-target") + 1] for argv in self.compiles("Selected") if "-target" in argv)
        self.assertEqual(targets, ["dxil", "dxil", "hlsl", "metal", "spirv"])

    def test_the_record_names_the_compiler_includes_and_flags(self) -> None:
        self.select("Selected")
        self.build()
        common = (f"slangc {self.work / 'bin' / 'slangc'} mock 1.0\n"
                  f"includes -I {ROOT / 'Shaders' / 'RojoRHI'}\n")
        self.assertEqual((self.out / "Selected.precision").read_text(),
                         common + "fp-mode -fp-mode precise\ndxc -Xdxc -Gis\nmetal -fno-fast-math -ffp-contract=off\n")
        self.assertEqual((self.out / "Ordinary.precision").read_text(), common + "fp-mode \ndxc \nmetal \n")

    def test_outputs_without_a_record_are_rebuilt_once(self) -> None:
        self.build()
        for record in self.out.glob("*.precision"):
            record.unlink()
        self.build()
        self.assert_precision("Ordinary", False)
        self.assert_precision("Selected", False)
        self.assertTrue((self.out / "Ordinary.precision").is_file())
        self.build()
        self.assertEqual(self.calls, [])

    def test_a_changed_selection_rebuilds_only_that_file(self) -> None:
        self.build()
        self.assert_precision("Selected", False)
        for names, precise in ((["Selected"], True), ([], False)):
            self.select(*names)
            self.build()
            self.assert_precision("Selected", precise)
            self.assertEqual(self.compiles("Ordinary"), [])
            self.build()
            self.assertEqual(self.calls, [])

    def test_a_name_that_is_no_entry_file_fails_before_any_compile(self) -> None:
        for name in ("Missing", "Selected.slang", "selected"):
            with self.subTest(name=name):
                self.select(name)
                output = self.build(succeeds=False)
                self.assertIn(f"slang.precise_files names '{name}'", output)
                self.assertEqual(self.calls, [])

    def test_a_failed_build_rebuilds_the_file_under_the_policy_selected_next(self) -> None:
        self.build()
        self.select("Selected")
        self.env["MOCK_FAIL"] = "Selected:spirv"
        self.assertIn("mock spirv failure", self.build(succeeds=False))
        self.assertFalse((self.out / "Selected.precision").exists())
        del self.env["MOCK_FAIL"]
        # The MSL of the failed build is precise; reverting must not keep it.
        self.select()
        self.build()
        self.assert_precision("Selected", False)
        self.assertIn("metal", [argv[argv.index("-target") + 1] for argv in self.compiles("Selected") if "-target" in argv])
        self.assertEqual(self.compiles("Ordinary"), [])


if __name__ == "__main__":
    unittest.main()
