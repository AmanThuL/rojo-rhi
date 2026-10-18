from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

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

    def test_missing_tool_probes_as_unknown(self) -> None:
        self.assertEqual(gpu_evidence.probe("rojorhi-no-such-tool"), "unknown")

    def test_main_checkout_of_a_worktree_is_the_main_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            main = Path(name).resolve() / "main"
            main.mkdir()
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
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


if __name__ == "__main__":
    unittest.main()
