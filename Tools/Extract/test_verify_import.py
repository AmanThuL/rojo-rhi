from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

import verify_import as verify


PATHS = verify.read_paths(verify.PATHS)
AUTHOR = ("Component Author", "author@example.invalid")
OTHER_AUTHOR = ("Someone Else", "someone@example.invalid")
DATES = ["2020-01-01T00:00:00+00:00", "2020-01-02T00:00:00+00:00", "2020-01-03T00:00:00+00:00"]

# A Luminex-shaped history: a commit that touches nothing the extraction keeps, a commit that adds
# the component under its old root, and the rename that moves it under RojoRHI/. The last commit
# carries the tag the extraction resets to.
SOURCE = [
    ("app: start the shell", DATES[0], {"Source/App/Shell.cpp": "shell\n"}),
    ("rhi: add the device (#7)", DATES[1], {
        "Source/App/Shell.cpp": "shell\n",
        "RHI/Include/rojoRHI/Device.h": "device\n",
        "Tests/RHIResultTests.cpp": "result\n",
    }),
    ("rhi: rename the component (#40)", DATES[2], {
        "Source/App/Shell.cpp": "shell\n",
        "RojoRHI/Include/rojoRHI/Device.h": "device\n",
        "RojoRHI/Tests/RHIResultTests.cpp": "result\n",
    }),
]
# The history a faithful extraction produces from it: the shell commit is gone, the component roots
# are stripped, and each pull-request number names the repository it belongs to.
COMPONENT = {"Include/rojoRHI/Device.h": "device\n", "Tests/RHIResultTests.cpp": "result\n"}
IMPORT = [
    ("rhi: add the device (AmanThuL/Luminex#7)", DATES[1], COMPONENT),
    ("rhi: rename the component (AmanThuL/Luminex#40)", DATES[2], COMPONENT),
]


def make_repo(root: Path, snapshots: list[tuple[str, str, dict]], author=AUTHOR) -> list[str]:
    """Create a repository whose commits hold exactly the given file maps; return their hashes."""
    root.mkdir(parents=True, exist_ok=True)
    name, email = author
    git = ["git", "-C", str(root), "-c", f"user.name={name}", "-c", f"user.email={email}",
           "-c", "commit.gpgsign=false"]
    subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
    shas = []
    for message, date, files in snapshots:
        for existing in sorted(root.rglob("*")):
            if existing.is_file() and ".git" not in existing.relative_to(root).parts:
                existing.unlink()
        for name_, text in files.items():
            (root / name_).parent.mkdir(parents=True, exist_ok=True)
            (root / name_).write_text(text, encoding="utf-8")
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "--allow-empty", "--date", date, "-m", message],
                       check=True, env={"GIT_AUTHOR_DATE": date, "PATH": "/usr/bin:/bin"})
        shas.append(verify.git(root, "rev-parse", "HEAD").strip())
    return shas


def write_commit_map(root: Path, pairs: list[tuple[str, str]]) -> None:
    """Fabricate the map filter-repo writes, header line and all, without running filter-repo."""
    directory = root / ".git" / "filter-repo"
    directory.mkdir(parents=True, exist_ok=True)
    lines = ["old                                      new"]
    lines += [f"{old} {new}" for old, new in pairs]
    (directory / "commit-map").write_text("\n".join(lines) + "\n", encoding="utf-8")


def build(directory: str, import_snapshots=None, author=AUTHOR):
    """The faithful source/import pair, with the import history overridable per test."""
    root = Path(directory)
    source, imported = root / "source", root / "import"
    source_shas = make_repo(source, SOURCE)
    subprocess.run(["git", "-C", str(source), "tag", verify.TAG], check=True)
    import_shas = make_repo(imported, import_snapshots or IMPORT, author=author)
    pairs = [(source_shas[0], verify.ZERO),
             (source_shas[1], import_shas[0]),
             (source_shas[2], import_shas[1])]
    write_commit_map(imported, pairs)
    return source, imported, pairs


class PathRuleTests(unittest.TestCase):
    def test_the_recorded_list_covers_the_component_roots(self) -> None:
        self.assertIn("RojoRHI/", PATHS)
        self.assertIn("RHI/", PATHS)
        self.assertIn("Source/RHI/", PATHS)

    def test_a_component_root_is_stripped_and_a_listed_file_keeps_its_path(self) -> None:
        self.assertEqual(verify.mapped("RojoRHI/Include/rojoRHI/Device.h"),
                         "Include/rojoRHI/Device.h")
        self.assertEqual(verify.mapped("RHI/Tests/RHIResultTests.cpp"), "Tests/RHIResultTests.cpp")
        self.assertEqual(verify.mapped("Source/RHI/RHI.h"), "Source/RHI/RHI.h")

    def test_an_unlisted_path_is_out_of_scope(self) -> None:
        self.assertTrue(verify.listed("RojoRHI/Include/rojoRHI/Device.h", PATHS))
        self.assertFalse(verify.listed("Source/App/Shell.cpp", PATHS))
        self.assertFalse(verify.listed("Tests/GpuTestSupport.h", PATHS))


class RuleOneTests(unittest.TestCase):
    def test_an_import_tree_that_differs_from_the_tag_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, _ = build(directory)
            stray = {**COMPONENT, "Include/rojoRHI/Stray.h": "stray\n"}
            make_repo(imported.parent / "broken", [("rhi: add a stray header", DATES[2], stray)])
            errors: list[str] = []
            verify.check_tree(source, imported.parent / "broken", errors)
            self.assertEqual(len(errors), 1)
            self.assertIn("main tree", errors[0])

    def test_the_faithful_tree_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, _ = build(directory)
            errors: list[str] = []
            verify.check_tree(source, imported, errors)
            self.assertEqual(errors, [])


class RuleTwoTests(unittest.TestCase):
    def test_a_file_lost_when_a_move_collapses_is_reported(self) -> None:
        lost = {"Include/rojoRHI/Device.h": "device\n"}
        with tempfile.TemporaryDirectory() as directory:
            snapshots = [(IMPORT[0][0], IMPORT[0][1], lost), IMPORT[1]]
            source, imported, pairs = build(directory, import_snapshots=snapshots)
            errors: list[str] = []
            verify.check_commit_trees(source, imported, pairs, PATHS, errors)
            self.assertEqual(len(errors), 1)
            self.assertIn("Tests/RHIResultTests.cpp", errors[0])

    def test_a_faithful_rewrite_of_every_commit_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, pairs = build(directory)
            errors: list[str] = []
            verify.check_commit_trees(source, imported, pairs, PATHS, errors)
            self.assertEqual(errors, [])


class RuleThreeTests(unittest.TestCase):
    def test_dropping_a_commit_that_touched_a_listed_path_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, _, pairs = build(directory)
            dropped = [(pairs[1][0], verify.ZERO)]
            errors: list[str] = []
            verify.check_dropped(source, dropped, PATHS, errors)
            self.assertEqual(len(errors), 2)
            self.assertIn("RHI/Include/rojoRHI/Device.h", errors[0])
            self.assertIn("Tests/RHIResultTests.cpp", errors[1])

    def test_dropping_a_commit_that_touched_nothing_listed_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, _, pairs = build(directory)
            errors: list[str] = []
            verify.check_dropped(source, pairs, PATHS, errors)
            self.assertEqual(errors, [])


class RuleFourTests(unittest.TestCase):
    def test_a_rewritten_author_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, pairs = build(directory, author=OTHER_AUTHOR)
            errors: list[str] = []
            verify.check_authors(source, imported, pairs, errors)
            self.assertEqual(len(errors), 2)
            self.assertTrue(all("author" in error for error in errors))

    def test_an_unchanged_author_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, pairs = build(directory)
            errors: list[str] = []
            verify.check_authors(source, imported, pairs, errors)
            self.assertEqual(errors, [])


class RuleFiveTests(unittest.TestCase):
    def test_a_message_that_still_names_a_bare_pull_request_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            snapshots = [(SOURCE[1][0], IMPORT[0][1], COMPONENT), IMPORT[1]]
            _, imported, pairs = build(directory, import_snapshots=snapshots)
            errors: list[str] = []
            verify.check_messages(imported, pairs, errors)
            self.assertEqual(len(errors), 1)
            self.assertIn("(#", errors[0])

    def test_qualified_pull_request_references_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            _, imported, pairs = build(directory)
            errors: list[str] = []
            verify.check_messages(imported, pairs, errors)
            self.assertEqual(errors, [])


class MainTests(unittest.TestCase):
    def test_a_faithful_import_exits_zero(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, _ = build(directory)
            status = verify.main(["--source", str(source), "--tag", verify.TAG,
                                  "--import", str(imported)])
            self.assertEqual(status, 0)

    def test_a_missing_commit_map_is_a_usage_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source, imported, _ = build(directory)
            (imported / ".git" / "filter-repo" / "commit-map").unlink()
            status = verify.main(["--source", str(source), "--tag", verify.TAG,
                                  "--import", str(imported)])
            self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
