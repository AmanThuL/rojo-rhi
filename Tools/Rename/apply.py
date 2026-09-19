#!/usr/bin/env python3
"""Apply the recorded Luminex-to-RojoRHI substitution in substitution.tsv to a Luminex checkout.

Exit status: 0 on success, 1 when --check or --selftest finds a failure, 2 on a usage or table
error or when a rewrite is refused.
"""

from __future__ import annotations

import argparse
import functools
import re
import subprocess
import sys
import tempfile
from pathlib import Path


TABLE = Path(__file__).with_name("substitution.tsv")
COLUMNS = ["order", "kind", "include", "exclude", "pattern", "replacement", "strict", "note"]
PROSE = ("docs/**", "**/*.md")
COMPONENT = "RojoRHI/"


class TableError(Exception):
    """The table is malformed; the message names the file and, where known, the line."""


class DirtyCheckoutError(Exception):
    """The checkout has uncommitted changes, so a rewrite could not be reproduced from it."""


class MoveError(Exception):
    """A `path` row cannot be applied, or its `git mv` failed; the message says how to recover."""


def read_table(path: Path) -> list[dict]:
    """Tab-split with no quoting: a double quote is ordinary field content.

    The bytes are decoded without newline translation and split on "\n" alone, after one
    trailing newline is dropped, so no other line-break character a field may hold ends a row.
    """
    text = path.read_bytes().decode("utf-8")
    lines = (text[:-1] if text.endswith("\n") else text).split("\n")
    if not lines or lines[0].split("\t") != COLUMNS:
        raise TableError(f"{path}: header must be {COLUMNS}")
    rows = []
    for number, line in enumerate(lines[1:], start=2):
        fields = line.split("\t")
        if len(fields) != len(COLUMNS):
            raise TableError(
                f"{path}:{number}: expected {len(COLUMNS)} fields, found {len(fields)}")
        row = dict(zip(COLUMNS, fields))
        try:
            row["order"] = int(row["order"])
        except ValueError:
            raise TableError(f"{path}:{number}: order {row['order']!r} is not an integer") from None
        if row["kind"] not in ("path", "regex"):
            raise TableError(f"{path}:{number}: kind {row['kind']!r} is neither path nor regex")
        if row["strict"] not in ("yes", "no"):
            raise TableError(f"{path}:{number}: strict {row['strict']!r} is neither yes nor no")
        if row["kind"] == "regex":
            if not row["include"]:
                raise TableError(f"{path}:{number}: a regex row needs an include glob")
            try:
                row["regex"] = re.compile(row["pattern"])
            except re.error as error:
                raise TableError(f"{path}:{number}: bad pattern: {error}") from None
        elif rows and rows[-1]["kind"] == "regex":
            raise TableError(f"{path}:{number}: a path row must precede every regex row, "
                             "because regex scopes are spelled in post-rename paths")
        rows.append(row)
    orders = [row["order"] for row in rows]
    if orders != sorted(set(orders)):
        raise TableError(f"{path}: orders must be strictly increasing")
    return rows


@functools.cache
def glob_to_re(glob: str) -> re.Pattern:
    """`**/` spans zero or more directories, `**` anything, `*` anything but a slash."""
    out, i = [], 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif glob.startswith("**", i):
            out.append(".*")
            i += 2
        elif glob[i] == "*":
            out.append("[^/]*")
            i += 1
        else:
            out.append(re.escape(glob[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def matches(globs, path: str) -> bool:
    return any(glob_to_re(glob).match(path) for glob in globs)


def in_scope(row: dict, path: str) -> bool:
    """Scopes are spelled in post-rename paths, because every path row runs first."""
    return matches(row["include"].split(), path) and not matches(row["exclude"].split(), path)


def load(root: Path) -> list[dict]:
    """One entry per tracked file; `text` is None for a file that is not UTF-8 text."""
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout.decode("utf-8")
    entries = []
    for name in sorted(name for name in listing.split("\0") if name):
        file = root / name
        text = None
        if file.is_file() and not file.is_symlink():
            try:
                text = file.read_bytes().decode("utf-8")
            except UnicodeDecodeError:
                text = None
        entries.append({"path": name, "text": text, "original": text})
    return entries


def moved(path: str, row: dict) -> str | None:
    """The path a `path` row moves `path` to, or None when the row does not cover it."""
    source = row["pattern"]
    if path == source or path.startswith(source + "/"):
        return row["replacement"] + path[len(source):]
    return None


def transform(entries: list[dict], rows: list[dict]) -> list[dict]:
    """Applies every row in memory, in order, and returns one report line per row.

    Each report line counts [matches, files] inside the component, outside it, and out of the
    row's scope; out-of-scope matches in prose are not counted.
    """
    report = []
    for row in rows:
        line = {"order": row["order"], "kind": row["kind"], "inside": [0, 0], "outside": [0, 0],
                "unscoped": [0, 0]}
        for entry in entries:
            if row["kind"] == "path":
                target = moved(entry["path"], row)
                if target is not None:
                    entry["path"] = target
                    line["inside"][0] += 1
                continue
            if entry["text"] is None:
                continue
            if in_scope(row, entry["path"]):
                entry["text"], count = row["regex"].subn(row["replacement"], entry["text"])
                bucket = "inside" if entry["path"].startswith(COMPONENT) else "outside"
            elif matches(PROSE, entry["path"]):
                continue
            else:
                count = len(row["regex"].findall(entry["text"]))
                bucket = "unscoped"
            if count:
                line[bucket][0] += count
                line[bucket][1] += 1
        report.append(line)
    return report


def verify_moves(root: Path, rows: list[dict]) -> None:
    """Before anything moves: each `path` row's source is tracked once the earlier rows have run,
    and its destination exists neither on disk nor among the tracked paths by then."""
    listing = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout.decode("utf-8")
    tracked = [name for name in listing.split("\0") if name]
    for row in rows:
        if row["kind"] != "path":
            continue
        source, destination = row["pattern"], row["replacement"]
        if not any(moved(name, row) is not None for name in tracked):
            raise MoveError(f"row {row['order']}: source {source} is not tracked in {root}")
        taken = {"pattern": destination, "replacement": destination}
        if (root / destination).exists() or any(moved(name, taken) for name in tracked):
            raise MoveError(f"row {row['order']}: destination {destination} already exists in "
                            f"{root}; git mv would move {source} inside it")
        tracked = [moved(name, row) or name for name in tracked]


def write(root: Path, entries: list[dict], rows: list[dict]) -> None:
    """Moves every `path` row's source with `git mv`, in order, then writes the changed texts."""
    status = subprocess.run(["git", "-C", str(root), "status", "--porcelain"], check=True,
                            capture_output=True).stdout
    if status:
        raise DirtyCheckoutError(f"{root} has uncommitted changes; refusing to rewrite it")
    verify_moves(root, rows)
    for row in rows:
        if row["kind"] == "path":
            command = ["git", "-C", str(root), "mv", row["pattern"], row["replacement"]]
            try:
                subprocess.run(command, check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as error:
                raise MoveError(
                    f"row {row['order']}: {' '.join(command)} failed:\n{error.stderr.strip()}\n"
                    f"earlier moves may have run; restore the checkout with "
                    f"`git -C {root} reset --hard && git -C {root} clean -fd` and re-run"
                ) from None
    for entry in entries:
        if entry["text"] != entry["original"]:
            (root / entry["path"]).write_bytes(entry["text"].encode("utf-8"))


def print_report(report: list[dict]) -> None:
    print("| row | inside component: matches / files | outside: matches / files "
          "| out of scope, not prose: matches / files |")
    print("|---|---|---|---|")
    for line in report:
        if line["kind"] == "path":
            print(f"| {line['order']} | {line['inside'][0]} paths moved | n/a | n/a |")
        else:
            cells = " | ".join(f"{found} / {files}" for found, files in
                               (line["inside"], line["outside"], line["unscoped"]))
            print(f"| {line['order']} | {cells} |")


def check(root: Path, rows: list[dict]) -> list[str]:
    """After a run: no path row's source is tracked, no regex row still matches in scope, and a
    strict row matches nowhere outside prose."""
    errors = []
    for entry in load(root):
        path = entry["path"]
        for row in rows:
            if row["kind"] == "path":
                if moved(path, row) is not None:
                    errors.append(f"row {row['order']}: {path} is still tracked")
                continue
            if entry["text"] is None or not row["regex"].search(entry["text"]):
                continue
            if in_scope(row, path):
                errors.append(f"row {row['order']}: pattern remains in {path}")
            elif row["strict"] == "yes" and not matches(PROSE, path):
                errors.append(f"row {row['order']} (strict): pattern found out of scope in {path}")
    return errors


# Self-test fixture: a Luminex-shaped tree before the rename, and the tree the real table must
# produce from it.
FIXTURE = {
    "RHI/Include/RHI/Device.h": "namespace lmx::rhi { struct Device; }\n",
    "RHI/Backends/Metal4/ImGui/Include/RHI/Metal4/Metal4ImGui.h": '#include "RHI/Device.h"\n',
    "RHI/Source/Base/Assert.h":
        '#define LMX_ASSERT(c) LMX_LOG_ERROR("x")\nconst char* k = "lmx.pass.unnamed";\n',
    "RHI/xmake/targets.lua":
        '-- keeps #include "RHI/..." working beside RHI/xmake.lua\ntarget("RHI")\n'
        'target("RHITests")\nadd_rules("rhi_slang2metallib")\n',
    "RHI/Tools/check_rhi_headers.py": 'H = INCLUDE.glob("RHI/**/*.h")\nI = ROOT / "RHI"\n',
    "RHI/Tools/ImGuiBufferProbe/main.mm": "#include LMX_IMGUI_BACKEND_SOURCE\n",
    "RHI/Tools/ImGuiBufferProbe/run.py":
        "# RHI/Tools/ImGuiBufferProbe/run.py\nD = f'-DLMX_IMGUI_BACKEND_SOURCE={s}'\n",
    "Source/App/Shell.cpp":
        '#include "RHI/Device.h"\nnamespace lmx::app {\nvoid f(rhi::Device&, lmx::rhi::Queue&);\n'
        'int a = sprhi::x + std::rhi::y;\nconst char* l = "lmx.pass.scene";\nLMX_ASSERT(1);\n}\n',
    "Tests/GpuTestSupport.h": '#include "../RHI/Tests/RhiGpuTestSupport.h"\n',
    "Tools/module_contract.json":
        '{"includeRoots": ["RHI/Include"], "asset": ["RHI/Format.h"], '
        '"shell": ["RHI/Metal4/Metal4ImGui.h"], "tests": ["RHI/Tests/RhiGpuTestSupport.h"], '
        '"targets": ["RHI", "RHIMetal4ImGui", "RHITests"], "forbidUndefined": "lmx::rhi::"}\n',
    "Tools/tests/test_x.py":
        'A = "RHI/Include/RHI/Device.h"\nB = "-IRHI/Include"\n'
        "C = 'RHI/Metal4/Metal4ImGui.h'\n"
        """D = '#include "RHI/Device.h"\\n#include "RHI/Source/Base/Log.h"'\n"""
        "# `RHI/RHI.h` names a header\n",
    "Tools/tests/test_module_deps.py":
        'T = {"RHI": {"deps": []}}\nE = [\n'
        '    "TextureBake: depends on RHI outside its allowed set",\n'
        '    "TextureBake: depends on Render outside its allowed set",\n]\n'
        'F = ["TextureBake: depends on RHI outside its allowed set"]\n',
    "docs/note.md": "RHI/ and lmx::rhi and RHITests stay in prose.\n"
                    "[RHI/Device.h](../../RHI/Include/RHI/Device.h) "
                    "[probe](../RHI/Tools/README.md)\n",
}
EXPECTED = {
    "RojoRHI/Include/rojoRHI/Device.h": "namespace rojoRHI { struct Device; }\n",
    "RojoRHI/Backends/Metal4/ImGui/Include/rojoRHI/Metal4/Metal4ImGui.h":
        "#include <rojoRHI/Device.h>\n",
    "RojoRHI/Source/Base/Assert.h":
        '#define ROJORHI_ASSERT(c) ROJORHI_LOG_ERROR("x")\n'
        'const char* k = "rojorhi.pass.unnamed";\n',
    "RojoRHI/xmake/targets.lua":
        '-- keeps #include <rojoRHI/...> working beside RojoRHI/xmake.lua\ntarget("RojoRHI")\n'
        'target("RojoRHITests")\nadd_rules("rojorhi_slang2metallib")\n',
    "RojoRHI/Tools/check_rhi_headers.py":
        'H = INCLUDE.glob("rojoRHI/**/*.h")\nI = ROOT / "RojoRHI"\n',
    "RojoRHI/Tools/ImGuiBufferProbe/main.mm": "#include ROJORHI_IMGUI_BACKEND_SOURCE\n",
    "RojoRHI/Tools/ImGuiBufferProbe/run.py":
        "# RojoRHI/Tools/ImGuiBufferProbe/run.py\nD = f'-DROJORHI_IMGUI_BACKEND_SOURCE={s}'\n",
    "Source/App/Shell.cpp":
        "#include <rojoRHI/Device.h>\nnamespace lmx::app {\n"
        "void f(rojoRHI::Device&, rojoRHI::Queue&);\n"
        'int a = sprhi::x + std::rhi::y;\nconst char* l = "lmx.pass.scene";\nLMX_ASSERT(1);\n}\n',
    "Tests/GpuTestSupport.h": '#include "../RojoRHI/Tests/RhiGpuTestSupport.h"\n',
    "Tools/module_contract.json":
        '{"includeRoots": ["RojoRHI/Include"], "asset": ["rojoRHI/Format.h"], '
        '"shell": ["rojoRHI/Metal4/Metal4ImGui.h"], '
        '"tests": ["RojoRHI/Tests/RhiGpuTestSupport.h"], '
        '"targets": ["RojoRHI", "RojoRHIMetal4ImGui", "RojoRHITests"], '
        '"forbidUndefined": "rojoRHI::"}\n',
    "Tools/tests/test_x.py":
        'A = "RojoRHI/Include/rojoRHI/Device.h"\nB = "-IRojoRHI/Include"\n'
        "C = 'rojoRHI/Metal4/Metal4ImGui.h'\n"
        """D = '#include "rojoRHI/Device.h"\\n#include "RojoRHI/Source/Base/Log.h"'\n"""
        "# `rojoRHI/RHI.h` names a header\n",
    "Tools/tests/test_module_deps.py":
        'T = {"RojoRHI": {"deps": []}}\nE = [\n'
        '    "TextureBake: depends on Render outside its allowed set",\n'
        '    "TextureBake: depends on RojoRHI outside its allowed set",\n]\n'
        'F = ["TextureBake: depends on RojoRHI outside its allowed set"]\n',
    "docs/note.md": "RHI/ and lmx::rhi and RHITests stay in prose.\n"
                    "[RHI/Device.h](../../RojoRHI/Include/rojoRHI/Device.h) "
                    "[probe](../RojoRHI/Tools/README.md)\n",
}


def selftest_reader(directory: Path) -> list[str]:
    """A field that opens and closes with a double quote keeps both quotes."""
    table = directory / "quoted.tsv"
    row = ["10", "regex", "**", "", '"RHI"', '"RojoRHI"', "no", 'a "quoted" note']
    table.write_text("\t".join(COLUMNS) + "\n" + "\t".join(row) + "\n", encoding="utf-8")
    found = read_table(table)[0]
    return [f"reader: {column}: expected {expected!r}, found {found[column]!r}"
            for column, expected in zip(COLUMNS[4:], row[4:]) if found[column] != expected]


def selftest(rows: list[dict]) -> int:
    """Applies the real table to FIXTURE in a throwaway repository and compares every file."""
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        failures += selftest_reader(Path(directory))
        root = Path(directory) / "checkout"
        git = ["git", "-C", str(root), "-c", "user.name=selftest",
               "-c", "user.email=selftest@example.invalid", "-c", "commit.gpgsign=false"]
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        for name, text in FIXTURE.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding="utf-8")
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "fixture"], check=True)
        entries = load(root)
        transform(entries, rows)
        write(root, entries, rows)
        subprocess.run(git + ["add", "-A"], check=True)
        found = {entry["path"]: entry["text"] for entry in load(root)}
        for name in sorted(set(EXPECTED) | set(found)):
            if EXPECTED.get(name) != found.get(name):
                failures.append(f"{name}: expected {EXPECTED.get(name)!r}, "
                                f"found {found.get(name)!r}")
        failures += check(root, rows)
        for line in transform(load(root), rows):
            if line["inside"] != [0, 0] or line["outside"] != [0, 0]:
                failures.append(f"row {line['order']} is not idempotent: {line}")
    print("\n".join(failures) if failures else f"selftest passed ({len(rows)} rows)")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", nargs="?", type=Path, help="Luminex checkout to rewrite")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="print per-row counts, write nothing")
    mode.add_argument("--check", action="store_true", help="verify a tree the table was applied to")
    mode.add_argument("--selftest", action="store_true",
                      help="apply the table to a built-in fixture and compare every file")
    args = parser.parse_args(argv)
    if args.selftest and args.root is not None:
        print("--selftest takes no checkout root", file=sys.stderr)
        return 2
    try:
        rows = read_table(TABLE)
    except TableError as error:
        print(error, file=sys.stderr)
        return 2
    if args.selftest:
        return selftest(rows)
    if args.root is None or not (args.root / ".git").exists():
        print("a Luminex checkout root is required", file=sys.stderr)
        return 2
    if args.check:
        errors = check(args.root, rows)
        print("\n".join(errors) if errors else "substitution check passed")
        return 1 if errors else 0
    entries = load(args.root)
    report = transform(entries, rows)
    if not args.dry_run:
        try:
            write(args.root, entries, rows)
        except (DirtyCheckoutError, MoveError) as error:
            print(error, file=sys.stderr)
            return 2
    print_report(report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
