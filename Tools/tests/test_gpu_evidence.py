from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import gpu_evidence


class GpuEvidenceTests(unittest.TestCase):
    def test_checksums_cover_every_file_but_themselves(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "b.txt").write_text("b")
            (directory / "a.txt").write_text("a")
            gpu_evidence.write_checksums(directory)
            lines = (directory / gpu_evidence.CHECKSUM_FILE).read_text().splitlines()
        expected = [f"{hashlib.sha256(text.encode()).hexdigest()}  {text}.txt" for text in "ab"]
        self.assertEqual(lines, expected)

    def test_recursive_checksums_name_nested_files_by_relative_path(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "b" / "c").mkdir(parents=True)
            (directory / "top.txt").write_text("t")
            (directory / "b" / "c" / "deep.bin").write_bytes(b"d")
            (directory / "b" / "mid.txt").write_text("m")
            gpu_evidence.write_checksums(directory, recursive=True)
            lines = (directory / gpu_evidence.CHECKSUM_FILE).read_text().splitlines()
        names = [line.split("  ", 1)[1] for line in lines]
        self.assertEqual(names, ["b/c/deep.bin", "b/mid.txt", "top.txt"])

    def test_recursive_checksums_skip_a_stale_checksum_file(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "a.txt").write_text("a")
            gpu_evidence.write_checksums(directory, recursive=True)
            gpu_evidence.write_checksums(directory, recursive=True)
            lines = (directory / gpu_evidence.CHECKSUM_FILE).read_text().splitlines()
        self.assertEqual(len(lines), 1)

    def test_record_keeps_the_log_and_returns_the_exit_code(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name) / "topic" / "2026-10-18"
            command = [sys.executable, "-c", "print('ran'); raise SystemExit(3)"]
            code = gpu_evidence.record(directory, "run", command, Path(name))
            log = (directory / "run.txt").read_text()
            names = sorted(path.name for path in directory.iterdir())
        self.assertEqual(code, 3)
        self.assertIn("ran\n", log)
        self.assertTrue(log.endswith("exit=3\n"))
        self.assertEqual(names, ["SHA256SUMS", "environment.json", "run.txt"])

    def test_record_checksums_files_the_command_wrote_in_subdirectories(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name) / "topic" / "2026-10-18"
            script = ("import pathlib; p = pathlib.Path(r'%s') / 'sub'; p.mkdir(parents=True); "
                      "(p / 'deep.bin').write_bytes(b'd')" % directory)
            code = gpu_evidence.record(directory, "run", [sys.executable, "-c", script], Path(name))
            lines = (directory / gpu_evidence.CHECKSUM_FILE).read_text().splitlines()
        names = [line.split("  ", 1)[1] for line in lines]
        self.assertEqual(code, 0)
        self.assertIn("sub/deep.bin", names)

    def test_missing_tool_probes_as_unknown(self) -> None:
        self.assertEqual(gpu_evidence.probe("rojorhi-no-such-tool"), "unknown")

    def test_main_checkout_of_a_worktree_is_the_main_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            main = Path(name).resolve() / "main"
            main.mkdir()
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "gc.auto=0",
                   "-c", "maintenance.auto=false", "-c", "gc.autoDetach=false"]
            subprocess.run([*git, "init", "-q"], cwd=main, check=True)
            subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "x"], cwd=main, check=True)
            work = Path(name).resolve() / "trees" / "w"
            subprocess.run([*git, "worktree", "add", "-q", "-b", "w", str(work)], cwd=main,
                           check=True)
            self.assertEqual(gpu_evidence.main_checkout(work), main)
            self.assertEqual(gpu_evidence.main_checkout(main), main)

    def test_main_checkout_falls_back_outside_a_repository(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            self.assertEqual(gpu_evidence.main_checkout(Path(name)), Path(name))


class WindowsEnvironmentTests(unittest.TestCase):
    def test_environment_on_windows_names_msvc_dxc_and_slang(self) -> None:
        answers = {"powershell": "Test CPU", "cl": "Microsoft (R) C/C++ 19.44",
                   "dxc.exe": "dxcompiler.dll: 1.8", "slangc.exe": "2026.14.1", "git": "abc123"}

        def probe(*command: str, any_status: bool = False) -> str:
            if command[0] == "powershell" and "VersionInfo" in command[-1]:
                return "1.619.6" if "D3D12Core" in command[-1] else "10.0.26100.1"
            return answers[Path(command[0]).name]

        with mock.patch.object(gpu_evidence.sys, "platform", "win32"), \
                mock.patch.object(gpu_evidence, "probe", probe), \
                mock.patch.object(gpu_evidence, "dirty", lambda: "no"), \
                mock.patch.object(gpu_evidence.platform, "version", lambda: "10.0.26100"), \
                mock.patch.object(gpu_evidence.platform, "machine", lambda: "AMD64"):
            found = gpu_evidence.environment()
        self.assertEqual(found, {
            "machine": "AMD64", "device": "Test CPU", "osVersion": "10.0.26100", "osBuild": "26100",
            "msvc": "Microsoft (R) C/C++ 19.44", "dxc": "dxcompiler.dll: 1.8",
            "slang": "2026.14.1", "agility": "1.619.6", "warp": "10.0.26100.1", "commit": "abc123", "dirty": "no"})

    def test_windows_cpu_falls_back_to_the_platform_processor(self) -> None:
        with mock.patch.object(gpu_evidence.sys, "platform", "win32"), \
                mock.patch.object(gpu_evidence, "probe", lambda *command, any_status=False: "unknown"), \
                mock.patch.object(gpu_evidence, "dirty", lambda: "no"), \
                mock.patch.object(gpu_evidence.platform, "processor", lambda: "Family 6"):
            self.assertEqual(gpu_evidence.environment()["device"], "Family 6")


if __name__ == "__main__":
    unittest.main()
