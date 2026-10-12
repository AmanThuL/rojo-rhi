#!/usr/bin/env python3
"""Check that every API reference entry has a signature, valid usage and a costed mapping."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = Path("docs/spec")
ENTRY = re.compile(r"^### `([^`]+)`\s*$")
VALID_USAGE = re.compile(r"^- `(VU-rojo-[a-z]+-\d{4})` \S")
VERSION = re.compile(r"(?m)^\*\*API version\*\*: \d+\.\d+\.\d+$")
COST = re.compile(r"^(native|cheap|costly|impossible)(?=$|:|\s)")
VALID_USAGE_HEADING = "#### Valid usage"
MAPPING_HEADER = "| | Metal 4 | D3D12 | Vulkan |"
NO_RULES = "- None."


def check_valid_usage(where: str, lines: list[str], ids: dict[str, str], errors: list[str]) -> None:
    start = lines.index(VALID_USAGE_HEADING) + 1
    end = next((i for i in range(start, len(lines)) if lines[i].startswith("###")), len(lines))
    bullets = [line for line in lines[start:end] if line.startswith("- ")]
    if not bullets:
        errors.append(f"{where}: empty valid usage section")
    for line in bullets:
        match = VALID_USAGE.match(line)
        if line == NO_RULES:
            continue
        if not match:
            errors.append(f"{where}: malformed valid usage line: {line}")
        elif match.group(1) in ids:
            errors.append(f"{where}: duplicate {match.group(1)} (first at {ids[match.group(1)]})")
        else:
            ids[match.group(1)] = where


def check_entry(where: str, body: list[str], ids: dict[str, str], errors: list[str]) -> None:
    lines = [line.rstrip() for line in body]
    if not any(line.startswith(("```cpp", "```slang")) for line in lines):
        errors.append(f"{where}: missing signature block")
    if VALID_USAGE_HEADING not in lines:
        errors.append(f"{where}: missing valid usage section")
    else:
        check_valid_usage(where, lines, ids, errors)
    if MAPPING_HEADER not in lines:
        errors.append(f"{where}: missing mapping table")
        return
    row = next((line for line in lines if line.startswith("| Cost |")), "")
    cells = [cell.strip() for cell in row.strip().strip("|").split("|")[1:]]
    if len(cells) != 3 or not all(COST.match(cell) for cell in cells):
        errors.append(f"{where}: Cost row needs one cost class per backend")


def check_chapters(chapters: list[tuple[str, str]], errors: list[str]) -> None:
    ids: dict[str, str] = {}
    names: dict[str, str] = {}
    for name, text in chapters:
        lines = text.splitlines()
        head = "\n".join(lines[:30])
        if not VERSION.search(head):
            errors.append(f"{name}: missing API version line")
        starts = [index for index, line in enumerate(lines) if ENTRY.match(line)]
        declares_none = "**Entries**: none" in head
        if not starts and not declares_none:
            errors.append(f"{name}: no entries")
        if starts and declares_none:
            errors.append(f"{name}: declares no entries but defines {len(starts)}")
        for position, start in enumerate(starts):
            end = starts[position + 1] if position + 1 < len(starts) else len(lines)
            entry = ENTRY.match(lines[start]).group(1)
            where = f"{name}:{start + 1}: {entry}"
            if entry in names:
                errors.append(f"{where}: duplicate entry {entry} (first at {names[entry]})")
            names.setdefault(entry, where)
            check_entry(where, lines[start:end], ids, errors)


def main() -> int:
    paths, errors = sorted((ROOT / SPEC).glob("*.md")), []
    check_chapters([(str(SPEC / p.name), p.read_text(encoding="utf-8")) for p in paths], errors)
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"spec check passed ({len(paths)} chapters)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
