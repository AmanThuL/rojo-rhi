#!/usr/bin/env python3
"""Verify that an imported RojoRHI history is exactly Luminex's, restricted to paths.txt.

Five rules hold together: the imported tip is the component's tree at the tag, every rewritten
commit holds exactly the listed files that its original held, no commit touching a listed path was
dropped, authorship is untouched, and no message still names a bare pull request.

Exit status: 0 when every rule holds, 1 with one line per failure, 2 on a usage error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


PATHS = Path(__file__).with_name("paths.txt")
TAG = "r2.4-pre-extraction-evidence"
COMPONENT = "RojoRHI/"
ROOTS = ("RojoRHI/", "RHI/")
ZERO = "0" * 40
SHA = re.compile(r"\A[0-9a-f]{40}\Z")
BARE_PULL_REQUEST = "(#"


class UsageError(Exception):
    """A repository, the commit map or the path list could not be read."""


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *args], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise UsageError(result.stderr.strip() or f"git {' '.join(args)} failed in {root}")
    return result.stdout


def read_paths(path: Path) -> list[str]:
    """One path per line, in the prefix form `git filter-repo --paths-from-file` reads."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UsageError(f"{path}: {error}") from None
    paths = [line.strip() for line in lines if line.strip()]
    if not paths:
        raise UsageError(f"{path}: no paths listed")
    return paths


def listed(path: str, paths: list[str]) -> bool:
    """filter-repo keeps a file whose name starts with a listed path, so this matches it."""
    return any(path.startswith(entry) for entry in paths)


def mapped(path: str) -> str:
    """Strip the component root the extraction renames away; any other path keeps its spelling."""
    for root in ROOTS:
        if path.startswith(root):
            return path[len(root):]
    return path


def read_commit_map(import_root: Path) -> list[tuple[str, str]]:
    """Read filter-repo's map; its header line and any short line are not a pair."""
    path = import_root / ".git" / "filter-repo" / "commit-map"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise UsageError(f"{path}: {error}") from None
    pairs = []
    for line in lines:
        fields = line.split()
        if len(fields) == 2 and SHA.match(fields[0]) and SHA.match(fields[1]):
            pairs.append((fields[0], fields[1]))
    if not pairs:
        raise UsageError(f"{path}: no commit pairs found")
    return pairs


def tree_entries(root: Path, commit: str) -> set[tuple[str, str]]:
    """(path, blob) for every file of a commit's tree, read with -z so no name is quoted."""
    raw = git(root, "ls-tree", "-r", "-z", commit)
    entries = set()
    for record in raw.split("\0"):
        if not record:
            continue
        header, _, name = record.partition("\t")
        entries.add((name, header.split()[2]))
    return entries


def check_tree(source: Path, import_root: Path, errors: list[str], tag: str = TAG) -> None:
    """Rule 1: the imported tip is the component's tree at the tag, file for file."""
    imported = git(import_root, "rev-parse", "main^{tree}").strip()
    expected = git(source, "rev-parse", f"{tag}:{COMPONENT.rstrip('/')}").strip()
    if imported != expected:
        errors.append(f"rule 1: imported main tree {imported} is not {tag}:{COMPONENT.rstrip('/')} "
                      f"{expected}")


def check_commit_trees(source: Path, import_root: Path, pairs: list[tuple[str, str]],
                       paths: list[str], errors: list[str]) -> None:
    """Rule 2: every rewritten commit holds exactly the listed files its original held.

    This is the rule that catches a file lost when a move collapses onto its own path: two source
    spellings that map to one imported path leave the mapped set short by whichever one lost.
    """
    for old, new in pairs:
        if new == ZERO:
            continue
        expected = {(mapped(name), blob) for name, blob in tree_entries(source, old)
                    if listed(name, paths)}
        found = tree_entries(import_root, new)
        for name, _ in sorted(expected - found):
            errors.append(f"rule 2: {old[:12]} -> {new[:12]}: {name} is missing or has other bytes")
        for name, _ in sorted(found - expected):
            errors.append(f"rule 2: {old[:12]} -> {new[:12]}: {name} is unexpected in the import")


def check_dropped(source: Path, pairs: list[tuple[str, str]], paths: list[str],
                  errors: list[str]) -> None:
    """Rule 3: a commit the rewrite dropped changed no listed path.

    `--root` includes the first commit, whose diff is otherwise empty. A merge prints no diff here,
    so a dropped merge is accepted: filter-repo drops one only when its parents already agree.
    """
    for old, new in pairs:
        if new != ZERO:
            continue
        changed = git(source, "diff-tree", "-r", "--root", "--no-commit-id", "--name-only", "-z",
                      old, "--", *paths)
        names = sorted(name for name in changed.split("\0") if name)
        for name in names:
            errors.append(f"rule 3: dropped commit {old[:12]} changed listed path {name}")


def check_authors(source: Path, import_root: Path, pairs: list[tuple[str, str]],
                  errors: list[str]) -> None:
    """Rule 4: author name, email and author date survive the rewrite."""
    for old, new in pairs:
        if new == ZERO:
            continue
        before = git(source, "log", "-1", "--format=%an%x00%ae%x00%aI", old).strip("\n")
        after = git(import_root, "log", "-1", "--format=%an%x00%ae%x00%aI", new).strip("\n")
        if before != after:
            errors.append(f"rule 4: {old[:12]} -> {new[:12]}: author changed from "
                          f"{before.replace(chr(0), ' | ')} to {after.replace(chr(0), ' | ')}")


def check_messages(import_root: Path, pairs: list[tuple[str, str]], errors: list[str]) -> None:
    """Rule 5: no rewritten message still names a pull request without its repository."""
    for old, new in pairs:
        if new == ZERO:
            continue
        message = git(import_root, "log", "-1", "--format=%B", new)
        if BARE_PULL_REQUEST in message:
            subject = message.splitlines()[0] if message.splitlines() else ""
            errors.append(f"rule 5: {new[:12]}: message still holds '{BARE_PULL_REQUEST}': "
                          f"{subject}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True, type=Path, help="the Luminex checkout to read")
    parser.add_argument("--tag", default=TAG, help="the Luminex tag the extraction started from")
    parser.add_argument("--import", required=True, dest="import_root", type=Path,
                        help="the filtered clone to verify")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    errors: list[str] = []
    try:
        paths = read_paths(PATHS)
        pairs = read_commit_map(args.import_root)
        check_tree(args.source, args.import_root, errors, tag=args.tag)
        check_commit_trees(args.source, args.import_root, pairs, paths, errors)
        check_dropped(args.source, pairs, paths, errors)
        check_authors(args.source, args.import_root, pairs, errors)
        check_messages(args.import_root, pairs, errors)
    except UsageError as error:
        print(f"import verification could not run: {error}", file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"import verification failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"import verification passed ({len(pairs)} mapped commits, {len(paths)} listed paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
