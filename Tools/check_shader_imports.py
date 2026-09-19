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
# Each Slang tree, paired with the directory holding its shared modules. The component has one
# tree: the test oracles under Shaders/Tests and the modules they share under Shaders/Tests/Modules.
TREES = (("Shaders", "Tests/Modules"),)


def without_comments(text: str) -> str:
    """Preserve strings and line numbers while masking both comment forms."""
    return TOKENS.sub(
        lambda match: match.group() if match.group().startswith('"') else re.sub(r"[^\n]", " ", match.group()),
        text,
    )


def check_shaders(root: Path) -> tuple[list[str], int, int]:
    """Check every Slang tree in the component, each in its own namespace.

    A tree is a shader root paired with the directory its shared modules live in. Each tree compiles
    into its own target directory, so output basenames are unique within a tree.
    """
    errors: list[str] = []
    files = imports = 0
    for tree, modules in TREES:
        shaders = root / tree
        if not shaders.is_dir():
            continue
        tree_errors, tree_files, tree_imports = check_tree(root, shaders, modules)
        errors += tree_errors
        files += tree_files
        imports += tree_imports
    return errors, files, imports


def check_tree(root: Path, shaders: Path, modules_dir: str) -> tuple[list[str], int, int]:
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
        if relative.is_relative_to(modules_dir):
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
        if path.relative_to(shaders).is_relative_to(modules_dir) and ENTRY.search(masked):
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
            if other is not None and not other.relative_to(shaders).is_relative_to(modules_dir):
                errors.append(
                    f"{relative}:{line}: import '{name}' reaches entry point "
                    f"{other.relative_to(root)}; imports may target "
                    f"{(shaders / modules_dir).relative_to(root)} only"
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
