#!/usr/bin/env python3
"""Package one shader's Metal compile outputs into a RojoRHI shader library.

The library is a text header followed by the raw metallib:

  rojorhi.metal4.shaderlibrary
  abi <N>
  entry <name> <stage> <x> <y> <z>        one line per entry point, in reflection order
  metallib <bytes>
  <bytes raw metallib bytes, then end of file>

Entry names and stages come from the Slang reflection of the MSL compile. Threadgroup sizes come from
the SPIR-V module of the same shader, whose `LocalSize` execution mode the compiler has resolved; a
compute entry must agree with the size the reflection reports. The ABI number comes from the shader
ABI module.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple


MAGIC_LINE = "rojorhi.metal4.shaderlibrary"
METALLIB_MAGIC = b"MTLB"
NAME_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
ABI_PATTERN = re.compile(r"\bpublic\s+static\s+const\s+uint\s+ROJORHI_SHADER_ABI_VERSION\s*=\s*([0-9]+)\s*;")
COMMENT_PATTERN = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

SPIRV_MAGIC = 0x07230203
SPIRV_HEADER_WORDS = 5
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
MODE_LOCAL_SIZE = 17
MODE_LOCAL_SIZE_ID = 38

STAGE_OF_REFLECTION = {
    "vertex": "vertex",
    "fragment": "fragment",
    "compute": "compute",
    "mesh": "mesh",
    "amplification": "task",
}
MODEL_OF_STAGE = {"vertex": 0, "fragment": 4, "compute": 5, "task": 5364, "mesh": 5365}
SIZED_STAGES = {"compute", "task", "mesh"}


class LibraryError(Exception):
    """An input that cannot be packaged into a shader library."""


class Reflected(NamedTuple):
    name: str
    stage: str
    thread_group_size: tuple[int, int, int] | None


class Entry(NamedTuple):
    name: str
    stage: str
    size: tuple[int, int, int]


def abi_version(module_text: str) -> int:
    matches = ABI_PATTERN.findall(COMMENT_PATTERN.sub("", module_text))
    if not matches:
        raise LibraryError("no 'public static const uint ROJORHI_SHADER_ABI_VERSION = <N>;' declaration")
    if len(matches) > 1:
        raise LibraryError("more than one ROJORHI_SHADER_ABI_VERSION declaration")
    version = int(matches[0])
    if version < 1:
        raise LibraryError("ROJORHI_SHADER_ABI_VERSION must be at least 1")
    return version


def reflected_entries(reflection: dict) -> list[Reflected]:
    if not isinstance(reflection, dict):
        raise LibraryError("the reflection is not a JSON object")
    items = reflection.get("entryPoints", [])
    if not isinstance(items, list):
        raise LibraryError("'entryPoints' is not a list")
    entries: list[Reflected] = []
    seen: set[str] = set()
    for item in items:
        if not isinstance(item, dict):
            raise LibraryError("an entry of 'entryPoints' is not an object")
        name = item.get("name", "")
        if not isinstance(name, str) or not NAME_PATTERN.fullmatch(name):
            raise LibraryError(f"entry name '{name}' is not [A-Za-z_][A-Za-z0-9_]*")
        if name in seen:
            raise LibraryError(f"entry '{name}' is repeated in the reflection")
        seen.add(name)
        reflected_stage = item.get("stage")
        stage = STAGE_OF_REFLECTION.get(reflected_stage) if isinstance(reflected_stage, str) else None
        if stage is None:
            raise LibraryError(f"entry '{name}' has unsupported stage '{item.get('stage')}'")
        size = item.get("threadGroupSize")
        if size is not None and not (
            isinstance(size, list) and len(size) == 3 and all(type(v) is int for v in size)
        ):
            raise LibraryError(f"entry '{name}' threadGroupSize is not a list of three integers")
        entries.append(Reflected(name, stage, tuple(size) if size is not None else None))
    if not entries:
        raise LibraryError("the reflection has no entry points")
    return entries


def read_words(spirv: bytes) -> list[int]:
    if len(spirv) % 4 != 0 or len(spirv) < SPIRV_HEADER_WORDS * 4:
        raise LibraryError(
            "SPIR-V module is truncated: not a whole number of words or shorter than its header"
        )
    words = list(struct.unpack(f"<{len(spirv) // 4}I", spirv))
    if words[0] != SPIRV_MAGIC:
        raise LibraryError(f"SPIR-V magic is 0x{words[0]:08x}, expected 0x{SPIRV_MAGIC:08x}")
    return words


def decode_name(words: list[int], at: int) -> str:
    data = struct.pack(f"<{len(words)}I", *words)
    end = data.find(b"\0")
    if end < 0:
        raise LibraryError(f"SPIR-V OpEntryPoint at word {at} has an unterminated name")
    try:
        return data[:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise LibraryError(f"SPIR-V OpEntryPoint at word {at} has a name that is not UTF-8") from error


def spirv_entries(spirv: bytes) -> dict[str, tuple[int, tuple[int, int, int] | None]]:
    words = read_words(spirv)
    models: dict[str, int] = {}
    ids: dict[int, str] = {}
    sizes: dict[int, tuple[int, int, int]] = {}
    at = SPIRV_HEADER_WORDS
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 0xFFFF
        if count == 0:
            raise LibraryError(f"SPIR-V instruction at word {at} has a zero word count")
        if at + count > len(words):
            raise LibraryError(
                f"SPIR-V instruction at word {at} is truncated: {count} words, {len(words) - at} left"
            )
        operands = words[at + 1 : at + count]
        if opcode == OP_ENTRY_POINT and len(operands) >= 3:
            name = decode_name(operands[2:], at)
            if name in models:
                raise LibraryError(f"SPIR-V module has more than one OpEntryPoint named '{name}'")
            models[name] = operands[0]
            ids[operands[1]] = name
        elif opcode == OP_EXECUTION_MODE and len(operands) >= 2:
            if operands[1] == MODE_LOCAL_SIZE_ID:
                raise LibraryError(
                    "a specialization-constant threadgroup size cannot be packaged (LocalSizeId)"
                )
            if operands[1] == MODE_LOCAL_SIZE and len(operands) >= 5:
                sizes[operands[0]] = (operands[2], operands[3], operands[4])
        at += count
    by_name = {name: sizes.get(function) for function, name in ids.items()}
    return {name: (model, by_name.get(name)) for name, model in models.items()}


def library_entries(
    reflected: list[Reflected], spirv: dict[str, tuple[int, tuple[int, int, int] | None]]
) -> list[Entry]:
    entries: list[Entry] = []
    for item in reflected:
        if item.name not in spirv:
            raise LibraryError(f"entry '{item.name}' is not an OpEntryPoint of the SPIR-V module")
        model, size = spirv[item.name]
        if model != MODEL_OF_STAGE[item.stage]:
            raise LibraryError(
                f"entry '{item.name}' is {item.stage} in the reflection "
                f"but has SPIR-V execution model {model}"
            )
        if item.stage not in SIZED_STAGES:
            entries.append(Entry(item.name, item.stage, (0, 0, 0)))
            continue
        if size is None:
            raise LibraryError(f"{item.stage} entry '{item.name}' has no LocalSize execution mode")
        if min(size) < 1:
            raise LibraryError(
                f"entry '{item.name}' LocalSize {' '.join(map(str, size))} must be at least 1 each"
            )
        if item.stage == "compute" and item.thread_group_size != size:
            reflected_size = " ".join(map(str, item.thread_group_size or ()))
            raise LibraryError(
                f"compute entry '{item.name}' has LocalSize {' '.join(map(str, size))} "
                f"but the reflection says threadGroupSize {reflected_size}"
            )
        entries.append(Entry(item.name, item.stage, size))
    return entries


def package(abi: int, entries: list[Entry], metallib: bytes) -> bytes:
    if not metallib.startswith(METALLIB_MAGIC):
        raise LibraryError("the metallib payload is not a metallib: it does not start with MTLB")
    lines = [MAGIC_LINE, f"abi {abi}"]
    lines += [f"entry {e.name} {e.stage} {e.size[0]} {e.size[1]} {e.size[2]}" for e in entries]
    lines.append(f"metallib {len(metallib)}")
    return ("\n".join(lines) + "\n").encode("ascii") + metallib


def read_input(path: Path, binary: bool) -> bytes | str:
    try:
        return path.read_bytes() if binary else path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise LibraryError(f"cannot read: {error}") from error


def build(module: Path, reflection: Path, spirv: Path, metallib: Path) -> bytes:
    try:
        abi = abi_version(read_input(module, False))
    except LibraryError as error:
        raise LibraryError(f"{module}: {error}") from error
    try:
        reflected = reflected_entries(json.loads(read_input(reflection, False)))
    except ValueError as error:
        raise LibraryError(f"{reflection}: {error}") from error
    except LibraryError as error:
        raise LibraryError(f"{reflection}: {error}") from error
    try:
        found = spirv_entries(read_input(spirv, True))
        entries = library_entries(reflected, found)
    except LibraryError as error:
        raise LibraryError(f"{spirv}: {error}") from error
    try:
        return package(abi, entries, read_input(metallib, True))
    except LibraryError as error:
        raise LibraryError(f"{metallib}: {error}") from error


def write_atomically(path: Path, data: bytes) -> None:
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--module", required=True, type=Path, help="Slang module holding the ABI version")
    parser.add_argument("--reflection", required=True, type=Path, help="-reflection-json of the MSL compile")
    parser.add_argument("--spirv", required=True, type=Path, help="SPIR-V compile of the same shader")
    parser.add_argument("--metallib", required=True, type=Path, help="metallib of the same shader")
    parser.add_argument("--out", required=True, type=Path, help="library to write")
    args = parser.parse_args(argv)
    try:
        data = build(args.module, args.reflection, args.spirv, args.metallib)
        write_atomically(args.out, data)
    except (LibraryError, OSError) as error:
        print(f"shader_library: {error}", file=sys.stderr)
        try:
            args.out.unlink(missing_ok=True)
        except OSError as unlink_error:
            print(f"shader_library: cannot remove {args.out}: {unlink_error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
