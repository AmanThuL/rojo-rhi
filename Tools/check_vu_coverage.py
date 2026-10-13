#!/usr/bin/env python3
"""Check that every valid-usage rule in the API reference has a death test or a pending entry."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = Path("docs/spec")
TESTS = Path("Tests")
PENDING = Path("Tools/vu_pending.txt")
PARTS = ("resources", "recording", "edges", "tracker")

DEFINITION = re.compile(r"^- `(VU-rojo-[a-z]+-\d{4})`\s*(.*)$")
TEST_CASE = re.compile(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"')
EXPECT_DEATH = re.compile(r'ROJORHI_EXPECT_DEATH\(\s*"([^"]*)"')
RAW_STRING = re.compile(r'R"([^()\\ ]{0,16})\(')
COVERED_NAME = re.compile(r"^(VU-rojo-[a-z]+-\d{4}) ")
IDENTIFIER = re.compile(r"^VU-rojo-[a-z]+-\d{4}$")


def spec_rules(root: Path, errors: list[str]) -> set[str]:
    rules: set[str] = set()
    for chapter in sorted((root / SPEC).glob("*.md")):
        for line in chapter.read_text(encoding="utf-8").splitlines():
            match = DEFINITION.match(line)
            if match and not match.group(2).startswith("Retired in"):
                rules.add(match.group(1))
    if not rules:
        errors.append(f"{SPEC}: no valid-usage rule found")
    return rules


def strip_comments(text: str) -> str:
    """Replace `//` and `/* */` comments with spaces, keeping newlines; string and character literals are left intact."""
    out: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        pair = text[i : i + 2]
        raw = RAW_STRING.match(text, i) if text[i] == "R" else None
        if raw:
            end = text.find(")" + raw.group(1) + '"', raw.end())
            end = length if end < 0 else end + len(raw.group(1)) + 2
            out.append(text[i:end])
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < length and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif pair == "//":
            while i < length and text[i] != "\n":
                i += 1
        elif pair == "/*":
            end = text.find("*/", i + 2)
            end = length if end < 0 else end + 2
            out.append("".join("\n" if c == "\n" else " " for c in text[i:end]))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def strip_dead_blocks(text: str) -> str:
    """Blank the lines of `#if 0` blocks, up to the matching `#else`, `#elif` or `#endif`."""
    out: list[str] = []
    depth = 0  # nesting depth of conditionals inside a dead block, 0 outside one
    dead = False
    for line in text.split("\n"):
        directive = re.match(r"\s*#\s*(\w+)\s*(\w*)", line)
        name = directive.group(1) if directive else ""
        if dead:
            if name in ("if", "ifdef", "ifndef"):
                depth += 1
            elif name == "endif":
                if depth == 0:
                    dead = False
                else:
                    depth -= 1
            elif name in ("else", "elif") and depth == 0:
                dead = False
            out.append("")
        elif name == "if" and directive.group(2) == "0":
            dead = True
            depth = 0
            out.append("")
        else:
            out.append(line)
    return "\n".join(out)


def covered_rules(root: Path, errors: list[str]) -> set[str]:
    covered: set[str] = set()
    for source in sorted((root / TESTS).rglob("*.cpp")):
        text = strip_dead_blocks(strip_comments(source.read_text(encoding="utf-8")))
        cases = list(TEST_CASE.finditer(text))
        for index, case in enumerate(cases):
            name, tags = case.group(1), case.group(2)
            match = COVERED_NAME.match(name)
            if not match or "[death]" not in tags:
                continue
            identifier = match.group(1)
            end = cases[index + 1].start() if index + 1 < len(cases) else len(text)
            arguments = EXPECT_DEATH.findall(text[case.end() : end])
            where = f"{source.relative_to(root)}: death test '{name}'"
            if not arguments:
                errors.append(f"{where} has no ROJORHI_EXPECT_DEATH(\"{identifier}\", ...) call")
                continue
            if any(argument != identifier for argument in arguments):
                errors.append(f"{where} expects {', '.join(sorted(set(arguments)))} but is named for {identifier}")
                continue
            covered.add(identifier)
    return covered


def pending_rules(root: Path, errors: list[str]) -> dict[str, str]:
    pending: dict[str, str] = {}
    path = root / PENDING
    if not path.is_file():
        errors.append(f"{PENDING}: missing")
        return pending
    previous = ""
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        where = f"{PENDING}:{number}"
        if len(fields) != 2 or not IDENTIFIER.match(fields[0]):
            errors.append(f"{where}: expected '<identifier> <part>', got: {line}")
            continue
        identifier, part = fields
        if part not in PARTS:
            errors.append(f"{where}: {identifier} names unknown part '{part}' (one of {', '.join(PARTS)})")
        if identifier in pending:
            errors.append(f"{where}: {identifier} is listed twice")
        if identifier < previous:
            errors.append(f"{where}: {identifier} is out of order; keep the list sorted by identifier")
        previous = max(previous, identifier)
        pending[identifier] = part
    return pending


def check(root: Path) -> tuple[list[str], str]:
    errors: list[str] = []
    rules = spec_rules(root, errors)
    covered = covered_rules(root, errors) & rules
    pending = pending_rules(root, errors)
    for identifier in sorted(pending):
        if identifier not in rules:
            errors.append(f"{identifier}: pending but not defined (or retired) in {SPEC}; remove it from the pending list")
        elif identifier in covered:
            errors.append(f"{identifier}: has a death test and is still pending; remove it from the pending list")
    for identifier in sorted(rules - covered - set(pending)):
        errors.append(f"{identifier}: no [death] test named for it and not pending; add a death test or a pending entry")
    open_count = len(rules & set(pending) - covered)
    return errors, f"vu coverage passed ({len(rules)} rules: {len(covered)} covered, {open_count} pending)"


def main() -> int:
    errors, summary = check(ROOT)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
