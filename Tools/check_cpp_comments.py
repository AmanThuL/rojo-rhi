#!/usr/bin/env python3
"""Validate project-owned C++ file envelopes and public API documentation."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from bisect import bisect_right
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("Include", "Source", "Backends")
# Test sources carry no file envelope; the Tests root already lies outside SOURCE_ROOTS.
EXCLUDED_ROOTS = ()
SOURCE_SUFFIXES = {".h", ".cpp"}
FILE_RULER = "//" + "-" * 118
# The ImGui adapter compiles only against a host's ImGui target, so a standalone compilation
# database never holds it. Its public headers are compiled whenever the database covers it.
ADAPTER_DIR = "Backends/Metal4/ImGui"


def project_cpp_files(root: Path) -> list[Path]:
    """Return project-owned headers and implementations, excluding test sources."""
    excluded = tuple((root / name).resolve() for name in EXCLUDED_ROOTS)
    files: list[Path] = []
    for name in SOURCE_ROOTS:
        source_root = root / name
        if source_root.is_dir():
            files.extend(
                path
                for path in source_root.rglob("*")
                if path.is_file()
                and path.suffix in SOURCE_SUFFIXES
                and not any(path.resolve().is_relative_to(base) for base in excluded)
            )
    return sorted(files)


def public_header_files(root: Path) -> list[Path]:
    """Return headers whose declarations form project API.

    The component exports two include trees, the core interfaces and the optional ImGui adapter;
    backend and shared implementation headers are excluded.
    """
    public: list[Path] = []
    for name in ("Include", "Backends/Metal4/ImGui/Include"):
        include_root = root / name
        if include_root.is_dir():
            public.extend(include_root.rglob("*.h"))
    return sorted(public)


def check_file_header(path: Path, text: str) -> list[str]:
    """Validate the exact four-line file envelope at the first physical line."""
    lines = text.splitlines()
    if len(lines) < 4:
        return [f"{path}:1: missing four-line file header envelope"]

    errors: list[str] = []
    if lines[0] != FILE_RULER:
        errors.append(f"{path}:1: missing 120-character file header ruler")
    if lines[1] != f"/// @file {path.name}":
        errors.append(f"{path}:2: expected `/// @file {path.name}`")
    if not lines[2].startswith("/// @brief ") or not lines[2][11:].strip():
        errors.append(f"{path}:3: expected a non-empty `/// @brief` summary")
    if lines[3] != FILE_RULER:
        errors.append(f"{path}:4: missing closing 120-character file header ruler")
    return errors


def _json_objects(text: str) -> list[dict[str, Any]]:
    """Decode Clang's concatenated JSON objects produced by ast-dump-filter."""
    decoder = json.JSONDecoder()
    objects: list[dict[str, Any]] = []
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        if isinstance(value, dict):
            objects.append(value)
    return objects


def _line_for(node: dict[str, Any], line_offsets: list[int]) -> int:
    loc = node.get("loc", {})
    if "line" in loc:
        return int(loc["line"])
    return bisect_right(line_offsets, int(loc.get("offset", 0))) + 1


def _has_comment(node: dict[str, Any]) -> bool:
    return any(child.get("kind") == "FullComment" for child in node.get("inner", []))


def _declaration_has_comment(node: dict[str, Any]) -> bool:
    if _has_comment(node):
        return True
    # Template nodes wrap the declaration that owns the parsed comment.
    return any(
        child.get("kind") != "FullComment" and _has_comment(child)
        for child in node.get("inner", [])
    )


def _is_definition(node: dict[str, Any]) -> bool:
    kind = node.get("kind")
    if kind in {"CXXRecordDecl", "RecordDecl"}:
        return node.get("completeDefinition") is True
    if kind == "EnumDecl":
        return any(child.get("kind") == "EnumConstantDecl" for child in node.get("inner", []))
    return True


def public_api_findings_from_ast(
    path: Path, text: str, ast_objects: list[dict[str, Any]], spelled: str | None = None
) -> list[str]:
    """Return undocumented API declarations from a Clang JSON AST.

    `spelled` is the header path as Clang was given it, when that differs from `path`.
    """
    target = spelled or path.as_posix()
    line_offsets = [index for index, char in enumerate(text) if char == "\n"]
    findings: list[str] = []
    seen: set[tuple[str, int, str]] = set()
    reported_lines: set[int] = set()
    declaration_kinds = {
        "CXXConstructorDecl",
        "CXXConversionDecl",
        "CXXDestructorDecl",
        "CXXMethodDecl",
        "FieldDecl",
        "FunctionDecl",
        "FunctionTemplateDecl",
        "TypeAliasDecl",
        "TypeAliasTemplateDecl",
        "TypedefDecl",
        "VarDecl",
    }

    def report(node: dict[str, Any], inherited_file: str | None) -> None:
        loc = node.get("loc", {})
        node_file = loc.get("file", inherited_file)
        if node_file is None or Path(node_file).as_posix() != target:
            return
        if node.get("isImplicit") or node.get("explicitlyDefaulted"):
            return
        line = _line_for(node, line_offsets)
        key = (str(node.get("kind")), int(loc.get("offset", -1)), str(node.get("name", "")))
        has_comment = (
            _declaration_has_comment(node)
            if str(node.get("kind", "")).endswith("TemplateDecl")
            else _has_comment(node)
        )
        if key in seen or line in reported_lines or has_comment:
            return
        seen.add(key)
        reported_lines.add(line)
        findings.append(f"{path}:{line}: public API declaration has no Doxygen comment")

    def visit(node: dict[str, Any], inherited_file: str | None, visible: bool) -> None:
        loc = node.get("loc", {})
        node_file = loc.get("file", inherited_file)
        kind = node.get("kind")
        children = node.get("inner", [])

        if kind in {"NamespaceDecl", "TranslationUnitDecl", "LinkageSpecDecl"}:
            for child in children:
                visit(child, node_file, visible)
            return

        if kind in {"CXXRecordDecl", "RecordDecl"}:
            if not _is_definition(node):  # Forward declarations are intentionally exempt.
                return
            if visible:
                report(node, node_file)
            access = "public" if node.get("tagUsed") == "struct" else "private"
            for child in children:
                if child.get("kind") == "AccessSpecDecl":
                    access = child.get("access", access)
                elif child.get("kind") not in {"FullComment", "CXXRecordDecl"} or child.get(
                    "completeDefinition"
                ):
                    visit(child, node_file, visible and access in {"public", "protected"})
            return

        if kind == "EnumDecl":
            if not _is_definition(node):
                return
            if visible:
                report(node, node_file)
                for child in children:
                    if child.get("kind") == "EnumConstantDecl":
                        report(child, node_file)
            return

        if kind in declaration_kinds and visible:
            report(node, node_file)
            return

        # Clang may insert template and export wrappers between a namespace and its declaration.
        if kind in {"ClassTemplateDecl", "ClassTemplateSpecializationDecl", "ExportDecl"}:
            if visible and kind.startswith("ClassTemplate"):
                report(node, node_file)
            for child in children:
                if child.get("kind") != "FullComment":
                    visit(child, node_file, visible)

    for ast in ast_objects:
        visit(ast, None, True)
    return sorted(findings)


def _compile_entries(path: Path) -> list[dict[str, Any]]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"cannot read compilation database {path}: {exc}") from exc


def _entry_path(entry: dict[str, Any]) -> Path:
    return (Path(entry["directory"]) / entry["file"]).resolve()


def component_entries(entries: list[dict[str, Any]], root: Path) -> list[dict[str, Any]]:
    """Return the entries for component sources; a host database also holds the host's own."""
    component = root.resolve()
    selected = [entry for entry in entries if _entry_path(entry).is_relative_to(component)]
    if not selected:
        raise RuntimeError(f"compilation database has no entry under the component root {root.name}")
    return selected


def api_headers(root: Path, entries: list[dict[str, Any]]) -> tuple[list[Path], list[str]]:
    """Return the public headers to compile, skipping the adapter only when the database lacks it."""
    headers = public_header_files(root)
    adapter = (root / ADAPTER_DIR).resolve()
    if any(_entry_path(entry).is_relative_to(adapter) for entry in entries):
        return headers, []
    kept = [path for path in headers if not path.resolve().is_relative_to(adapter)]
    if len(kept) == len(headers):
        return headers, []
    return kept, [f"{ADAPTER_DIR} skipped: the compilation database has no entry for the ImGui adapter"]


def _entry_for_header(header: Path, entries: list[dict[str, Any]], root: Path) -> dict[str, Any]:
    # The component is one module, so every compilation entry is a candidate.
    candidates = entries
    if not candidates:
        raise RuntimeError("compilation database is empty")
    stem_matches = [entry for entry in candidates if Path(entry["file"]).stem == header.stem]
    return (stem_matches or candidates)[0]


def _ast_for_header(
    header: Path, entries: list[dict[str, Any]], root: Path
) -> tuple[list[dict[str, Any]], str, str | None]:
    """Compile a header with its entry's flags from the entry's directory.

    Returns the AST, the header path as Clang was given it, and an error when compilation failed.
    """
    entry = _entry_for_header(header, entries, root)
    directory = entry["directory"]
    spelled = Path(os.path.relpath(header, directory)).as_posix()
    arguments = list(entry.get("arguments", []))
    if not arguments:
        return [], spelled, "compilation database entry has no argument vector"
    source = entry["file"]
    command: list[str] = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
        elif argument != "-c" and argument != source:
            command.append(argument)
    command.extend(
        [
            "-x",
            "c++",
            spelled,
            "-fsyntax-only",
            "-fparse-all-comments",
            "-Wdocumentation",
            "-Werror=documentation",
            "-Xclang",
            "-ast-dump=json",
            "-Xclang",
            # Every component declaration, including the ImGui adapter's, lives in rojoRHI.
            "-ast-dump-filter=rojoRHI",
        ]
    )
    result = subprocess.run(command, cwd=directory, capture_output=True, text=True, check=False)
    if result.returncode:
        return [], spelled, result.stderr.strip() or "Clang failed without diagnostics"
    try:
        return _json_objects(result.stdout), spelled, None
    except json.JSONDecodeError as exc:
        return [], spelled, f"cannot decode Clang AST: {exc}"


def compiler_public_api_findings(root: Path, compile_commands: Path) -> tuple[list[str], list[str]]:
    """Compile exported headers and report missing or malformed public API documentation.

    Returns the findings and any notices about headers the database cannot compile.
    """
    entries = component_entries(_compile_entries(compile_commands), root)
    headers, notices = api_headers(root, entries)
    findings: list[str] = []
    for header in headers:
        relative = header.relative_to(root)
        ast, spelled, error = _ast_for_header(header, entries, root)
        if error:
            findings.append(f"{relative}: public-header documentation compile failed:\n{error}")
            continue
        text = header.read_text(encoding="utf-8")
        findings.extend(public_api_findings_from_ast(relative, text, ast, spelled))
    return findings, notices


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument(
        "--compile-commands",
        type=Path,
        help="path to compile_commands.json (default: <component root>/compile_commands.json)",
    )
    parser.add_argument(
        "--public-api-docs",
        choices=("off", "warn", "error"),
        default="error",
        help="handling for undocumented or malformed public API declarations",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    files = project_cpp_files(root)
    errors: list[str] = []
    warnings: list[str] = []
    if not files:
        errors.append(f"{', '.join(SOURCE_ROOTS)}: no owned C++ sources found")
    for path in files:
        relative = path.relative_to(root)
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            errors.append(f"{relative}: cannot read source: {exc}")
            continue
        errors.extend(check_file_header(relative, text))

    if args.public_api_docs != "off":
        compile_commands = (args.compile_commands or root / "compile_commands.json").resolve()
        try:
            findings, notices = compiler_public_api_findings(root, compile_commands)
        except RuntimeError as exc:
            findings, notices = [str(exc)], []
        for notice in notices:
            print(f"notice: {notice}")
        (warnings if args.public_api_docs == "warn" else errors).extend(findings)

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"C++ comment check failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"C++ comment check passed ({len(files)} owned files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
