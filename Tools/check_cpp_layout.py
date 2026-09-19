#!/usr/bin/env python3
"""Validate C++ function boundaries using clangd's parsed document symbols."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, BinaryIO


ROOT = Path(__file__).resolve().parents[1]
CPP_ROOTS = ("Source", "Backends", "Tests", "Tools")
FUNCTION_SYMBOL_KINDS = {6, 9, 12}  # Method, Constructor, Function (LSP SymbolKind)
RULER = re.compile(r"^\s*//.*(?:={8,}|-{8,})")
SEPARATOR_BODY = re.compile(r"^//={100,}$")
FILE_RULER = "//" + "-" * 118
# The ImGui adapter compiles only against a host's ImGui target, so a standalone compilation
# database never holds it. It is checked whenever the database covers it.
ADAPTER_DIR = "Backends/Metal4/ImGui"


class LayoutSetupError(RuntimeError):
    """The checker could not obtain a trustworthy compiler view of the sources."""


def project_cpp_files(root: Path) -> list[Path]:
    return sorted(path for name in CPP_ROOTS for path in (root / name).rglob("*.cpp"))


def compilation_database_files(database: Path) -> set[Path]:
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise LayoutSetupError(
            "compile_commands.json is missing; run `xmake project -k compile_commands` first"
        ) from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise LayoutSetupError(f"cannot read compile_commands.json: {exc}") from exc

    files: set[Path] = set()
    for entry in entries:
        try:
            directory = Path(entry["directory"])
            source = Path(entry["file"])
        except (KeyError, TypeError) as exc:
            raise LayoutSetupError("compile_commands.json contains a malformed entry") from exc
        files.add((directory / source).resolve() if not source.is_absolute() else source.resolve())
    return files


def select_checked_files(
    files: list[Path], covered: set[Path], root: Path
) -> tuple[list[Path], list[str]]:
    """Drop the ImGui adapter, with a notice, only when the database has no entry under it."""
    adapter = (root / ADAPTER_DIR).resolve()
    if any(path.is_relative_to(adapter) for path in covered):
        return files, []
    kept = [path for path in files if not path.resolve().is_relative_to(adapter)]
    if len(kept) == len(files):
        return files, []
    return kept, [f"{ADAPTER_DIR} skipped: the compilation database has no entry for the ImGui adapter"]


def ensure_database_covers(files: list[Path], database: Path, root: Path) -> None:
    covered = compilation_database_files(database)
    missing = [path for path in files if path.resolve() not in covered]
    if not missing:
        return
    names = ", ".join(path.relative_to(root).as_posix() for path in missing)
    raise LayoutSetupError(
        "compile_commands.json does not cover every project .cpp file "
        f"({names}); rerun `xmake project -k compile_commands`"
    )


def flatten_symbols(symbols: list[dict[str, Any]]) -> list[dict[str, Any]]:
    flattened: list[dict[str, Any]] = []
    pending = list(symbols)
    while pending:
        symbol = pending.pop()
        flattened.append(symbol)
        pending.extend(symbol.get("children", []))
    return flattened


def definition_start_lines(
    symbols: list[dict[str, Any]], uri: str, definition_token_lines: set[int]
) -> set[int]:
    """Return definition starts from document symbols plus semantic definition tokens."""
    starts: set[int] = set()
    for symbol in flatten_symbols(symbols):
        if symbol.get("kind") not in FUNCTION_SYMBOL_KINDS:
            continue

        # clangd normally returns flat SymbolInformation with location. The hierarchical
        # DocumentSymbol form uses range directly.
        location = symbol.get("location")
        if location is not None:
            if location.get("uri") != uri:
                continue
            source_range = location.get("range")
        else:
            source_range = symbol.get("range")
        if not source_range:
            continue
        start = int(source_range["start"]["line"])
        end = int(source_range["end"]["line"])
        name = str(symbol.get("name", ""))
        # Catch2 emits a declaration and definition at the TEST_CASE macro location. Those
        # generated function names have no source token, so semantic tokens cannot mark them.
        is_catch_unit = name.startswith("CATCH2_INTERNAL_TEST_")
        if is_catch_unit or any(start <= line <= end for line in definition_token_lines):
            starts.add(start)
    return starts


def semantic_definition_lines(data: list[int], definition_modifier: int) -> set[int]:
    """Decode LSP's delta-encoded semantic token stream."""
    if len(data) % 5:
        raise LayoutSetupError("clangd returned a malformed semantic token stream")
    lines: set[int] = set()
    line = 0
    column = 0
    definition_bit = 1 << definition_modifier
    for index in range(0, len(data), 5):
        line_delta, column_delta, _length, _token_type, modifiers = data[index : index + 5]
        line += line_delta
        column = column_delta if line_delta else column + column_delta
        if modifiers & definition_bit:
            lines.add(line)
    return lines


def expected_separator(indent: str) -> str:
    columns = len(indent.expandtabs(4))
    if columns > 117:
        raise ValueError("function indentation leaves no room for a 120-column separator")
    return indent + "//" + "=" * (118 - columns)


def definition_prefix_start(lines: list[str], start: int) -> int:
    """Include declaration prefixes clangd omits from a function symbol's range."""
    first = start
    while first > 0:
        previous = lines[first - 1].lstrip()
        if previous.startswith("template ") or previous.startswith("template<"):
            first -= 1
            continue
        if previous.startswith("[["):
            first -= 1
            continue
        break
    return first


def check_layout(path: Path, text: str, starts: set[int]) -> list[str]:
    lines = text.splitlines()
    errors: list[str] = []
    claimed: set[int] = set()

    for start in sorted(starts):
        if start >= len(lines):
            errors.append(f"{path}:{start + 1}: compiler symbol starts beyond end of file")
            continue

        first = definition_prefix_start(lines, start)
        while (
            first > 0
            and lines[first - 1].lstrip().startswith("//")
            and not RULER.match(lines[first - 1])
        ):
            first -= 1
        boundary = first - 1
        indent = re.match(r"[ \t]*", lines[start]).group()
        expected = expected_separator(indent)
        if boundary < 0 or lines[boundary] != expected:
            errors.append(f"{path}:{start + 1}: missing 120-character function separator")
            continue
        claimed.add(boundary)

    separators: set[int] = set()
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if SEPARATOR_BODY.fullmatch(stripped):
            separators.add(index)
            indent = line[: len(line) - len(stripped)]
            if line != expected_separator(indent) or len(line) != 120:
                errors.append(f"{path}:{index + 1}: malformed function separator")
        elif RULER.match(line) and not (index in {0, 3} and line == FILE_RULER):
            errors.append(f"{path}:{index + 1}: alternate decorative ruler")

    for index in sorted(separators - claimed):
        errors.append(f"{path}:{index + 1}: orphan function separator")
    return errors


class ClangdClient:
    def __init__(self, executable: str, root: Path, database: Path) -> None:
        self.root = root
        self._next_id = 0
        self._process = subprocess.Popen(
            [
                executable,
                f"--compile-commands-dir={database.parent}",
                "--background-index=false",
                "--clang-tidy=false",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        if self._process.stdin is None or self._process.stdout is None:
            raise LayoutSetupError("could not open clangd's LSP streams")
        self._input: BinaryIO = self._process.stdin
        self._output: BinaryIO = self._process.stdout
        self._definition_modifier: int | None = None

    def _send(self, method: str, params: dict[str, Any], request: bool = True) -> int | None:
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method, "params": params}
        if request:
            self._next_id += 1
            message["id"] = self._next_id
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self._input.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
        self._input.write(payload)
        self._input.flush()
        return self._next_id if request else None

    def _receive(self, request_id: int) -> dict[str, Any]:
        while True:
            headers: dict[str, str] = {}
            while True:
                line = self._output.readline()
                if not line:
                    raise LayoutSetupError("clangd exited before replying")
                if line in (b"\n", b"\r\n"):
                    break
                key, value = line.decode("ascii").split(":", 1)
                headers[key.casefold()] = value.strip()
            try:
                length = int(headers["content-length"])
            except (KeyError, ValueError) as exc:
                raise LayoutSetupError("clangd sent an invalid LSP header") from exc
            message = json.loads(self._output.read(length))
            if message.get("id") == request_id:
                return message

    def request(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        request_id = self._send(method, params)
        assert request_id is not None
        response = self._receive(request_id)
        if "error" in response:
            raise LayoutSetupError(f"clangd rejected {method}: {response['error'].get('message')}")
        return response

    def notify(self, method: str, params: dict[str, Any]) -> None:
        self._send(method, params, request=False)

    def initialize(self) -> None:
        response = self.request(
            "initialize",
            {"processId": None, "rootUri": self.root.as_uri(), "capabilities": {}},
        )
        legend = (
            response.get("result", {})
            .get("capabilities", {})
            .get("semanticTokensProvider", {})
            .get("legend", {})
        )
        modifiers = legend.get("tokenModifiers", [])
        if "definition" not in modifiers:
            raise LayoutSetupError("clangd does not expose semantic definition tokens")
        self._definition_modifier = modifiers.index("definition")
        self.notify("initialized", {})

    def definition_lines(self, path: Path, text: str) -> set[int]:
        uri = path.as_uri()
        self.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "cpp",
                    "version": 1,
                    "text": text,
                }
            },
        )
        symbols = self.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        tokens = self.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
        self.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        assert self._definition_modifier is not None
        token_lines = semantic_definition_lines(
            (tokens.get("result") or {}).get("data", []), self._definition_modifier
        )
        return definition_start_lines(symbols.get("result") or [], uri, token_lines)

    def close(self) -> None:
        if self._process.poll() is not None:
            return
        try:
            self.request("shutdown", {})
            self.notify("exit", {})
            self._process.wait(timeout=5)
        except (LayoutSetupError, subprocess.TimeoutExpired):
            self._process.terminate()
            self._process.wait(timeout=5)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    parser.add_argument(
        "--compile-commands",
        type=Path,
        help="path to compile_commands.json (default: <component root>/compile_commands.json)",
    )
    parser.add_argument("--clangd", help="clangd executable (defaults to CLANGD/PATH)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    database = (args.compile_commands or root / "compile_commands.json").resolve()
    executable = args.clangd or shutil.which("clangd")
    if not executable:
        print("C++ layout check could not run: clangd is not on PATH", file=sys.stderr)
        return 2

    files = project_cpp_files(root)
    if not files:
        print(f"error: {', '.join(CPP_ROOTS)}: no C++ sources found", file=sys.stderr)
        print("C++ layout check failed with 1 error(s)", file=sys.stderr)
        return 1
    client: ClangdClient | None = None
    try:
        files, notices = select_checked_files(files, compilation_database_files(database), root)
        for notice in notices:
            print(f"notice: {notice}")
        ensure_database_covers(files, database, root)
        client = ClangdClient(executable, root, database)
        client.initialize()
        errors: list[str] = []
        definitions = 0
        for path in files:
            text = path.read_text(encoding="utf-8")
            starts = client.definition_lines(path, text)
            definitions += len(starts)
            errors.extend(check_layout(path.relative_to(root), text, starts))
    except (OSError, LayoutSetupError, ValueError) as exc:
        message = str(exc).replace(str(root), ".")
        print(f"C++ layout check could not run: {message}", file=sys.stderr)
        return 2
    finally:
        if client is not None:
            client.close()

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"C++ layout check failed with {len(errors)} error(s)", file=sys.stderr)
        return 1
    print(f"C++ layout check passed ({definitions} definitions in {len(files)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
