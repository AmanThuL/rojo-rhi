#!/usr/bin/env python3
"""Check Slang import boundaries and the flat runtime shader artifact namespace."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.DOTALL)
IMPORT = re.compile(r"\bimport\b([^;]*)(?:;|$)")
MODULE_NAME = re.compile(r"[A-Za-z_]\w*(?:(?:::|\.)[A-Za-z_]\w*)*")
ENTRY = re.compile(r'\[\s*shader\s*\(')
INCLUDE = re.compile(r"^\s*#\s*include\b", re.MULTILINE)
# Each Slang tree, paired with the directories holding its shared modules. The component has two
# trees: the frozen v1 test oracles under Legacy/Shaders/Tests with the modules they share under
# Legacy/Shaders/Tests/Modules, and the interface's module under Shaders/RojoRHI, with the test
# modules under Shaders/Tests/Modules, imported by the entry files under Shaders/Tests and
# Shaders/Bench.
TREES = (("Legacy/Shaders", ("Tests/Modules",)), ("Shaders", ("RojoRHI", "Tests/Modules")))


def without_comments(text: str) -> str:
    """Preserve strings and line numbers while masking both comment forms."""
    return TOKENS.sub(
        lambda match: match.group() if match.group().startswith('"') else re.sub(r"[^\n]", " ", match.group()),
        text,
    )


STRUCT = re.compile(r"\bstruct\s+(\w+)\s*\{(.*?)\}\s*;", re.DOTALL)
BENCH_PROBE = "Bench/BenchFrameProbe.slang"


def struct_bodies(path: Path) -> dict[str, str]:
    """The whitespace-normalised body of every struct in a Slang file, comments removed."""
    text = without_comments(path.read_text(encoding="utf-8"))
    return {match.group(1): " ".join(match.group(2).split()) for match in STRUCT.finditer(text)}


def check_probe_copies(root: Path, shaders: Path) -> list[str]:
    """Every struct a Bench entry file shares with the layout probe must match the probe's text.

    Textual includes are banned, so each entry file repeats the structs it reads; only the probe is
    compared with the C++ layout, so a drifting copy would go unnoticed without this check.
    """
    probe = shaders / BENCH_PROBE
    entries = sorted((shaders / "Bench").glob("Bench*.slang"))
    if not probe.is_file():
        if entries:
            return [f"{probe.relative_to(root)}: the layout probe is missing, so the structs of {len(entries)} Bench shaders are unchecked"]
        return []
    reference = struct_bodies(probe)
    errors: list[str] = []
    for path in entries:
        if path == probe:
            continue
        for name, body in struct_bodies(path).items():
            if name in reference and reference[name] != body:
                errors.append(f"{path.relative_to(root)}: struct {name} differs from {probe.relative_to(root)}")
    return errors


def check_shaders(root: Path) -> tuple[list[str], int, int]:
    """Check every Slang tree in the component, each in its own namespace.

    A tree is a shader root paired with the directories its shared modules live in. Each tree compiles
    into its own target directory, so output basenames are unique within a tree.
    """
    errors: list[str] = []
    files = imports = 0
    for tree, modules in TREES:
        shaders = root / tree
        if not shaders.is_dir():
            continue
        tree_errors, tree_files, tree_imports = check_tree(root, shaders, modules)
        tree_errors += check_probe_copies(root, shaders)
        errors += tree_errors
        files += tree_files
        imports += tree_imports
    return errors, files, imports


def module_directory(relative: Path, modules_dirs: tuple[str, ...]) -> str | None:
    """The module directory holding a file of the tree, or None for an entry file."""
    for modules_dir in modules_dirs:
        if relative.is_relative_to(modules_dir):
            return modules_dir
    return None


def check_tree(root: Path, shaders: Path, modules_dirs: tuple[str, ...]) -> tuple[list[str], int, int]:
    files = sorted(shaders.rglob("*.slang"))
    errors: list[str] = []
    names: dict[str, Path] = {}
    modules: dict[str, Path] = {}
    for path in files:
        relative = path.relative_to(shaders)
        key = path.stem.casefold()
        if key in names:
            errors.append(
                f"{path.relative_to(root)}: duplicate shader output basename '{path.stem}' "
                f"also owned by {names[key].relative_to(root)}"
            )
        names[key] = path
        modules_dir = module_directory(relative, modules_dirs)
        if modules_dir is not None:
            name = ".".join(relative.relative_to(modules_dir).with_suffix("").parts)
            modules[name] = path

    imports = 0
    for path in files:
        relative = path.relative_to(root)
        text = without_comments(path.read_text(encoding="utf-8"))
        # Strings cannot contain directives, but a quoted import operand must still be rejected
        # explicitly instead of disappearing while strings are masked. Preserve line numbers.
        masked = re.sub(
            r'"(?:\\.|[^"\\])*"', lambda match: re.sub(r"[^\n]", "?", match.group()), text
        )
        if module_directory(path.relative_to(shaders), modules_dirs) is not None and ENTRY.search(masked):
            errors.append(f"{relative}: shared modules cannot declare shader entry points")
        for match in INCLUDE.finditer(masked):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{relative}:{line}: use module imports instead of textual includes")
        for match in IMPORT.finditer(masked):
            imports += 1
            line = text.count("\n", 0, match.start()) + 1
            name = match.group(1).strip()
            if not MODULE_NAME.fullmatch(name) or not match.group().endswith(";"):
                errors.append(f"{relative}:{line}: expected a named module import ending in ';'")
                continue
            name = name.replace("::", ".")
            if name in modules:
                continue
            other = names.get(name.split(".")[-1].casefold())
            if other is not None and module_directory(other.relative_to(shaders), modules_dirs) is None:
                targets = " or ".join(str((shaders / modules_dir).relative_to(root)) for modules_dir in modules_dirs)
                errors.append(
                    f"{relative}:{line}: import '{name}' reaches entry point "
                    f"{other.relative_to(root)}; imports may target {targets} only"
                )
            else:
                errors.append(f"{relative}:{line}: unresolved module import '{name}'")
    return errors, len(files), imports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        errors, files, imports = check_shaders(args.root.resolve())
    except (OSError, UnicodeError) as exc:
        print(f"shader import check could not run: {exc}", file=sys.stderr)
        return 2
    if not files:
        errors.append("Shaders: no Slang sources found")
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors:
        print(f"shader import check failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"shader import check passed ({files} shaders, {imports} module imports)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
