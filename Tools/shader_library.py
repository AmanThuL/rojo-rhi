#!/usr/bin/env python3
"""Package one shader's compile outputs into a RojoRHI shader library, for Metal 4, D3D12 or the Null backend.

The Metal 4 library (--format metal4, the default) is a text header followed by the raw metallib:

  rojorhi.metal4.shaderlibrary
  abi <N>
  entry <name> <stage> <x> <y> <z>        one line per entry point, in reflection order
  metallib <bytes>
  <bytes raw metallib bytes, then end of file>

Entry names and stages come from the Slang reflection of the MSL compile. Threadgroup sizes come from
the SPIR-V module of the same shader, whose `LocalSize` execution mode the compiler has resolved; a
compute entry must agree with the size the reflection reports. The ABI number comes from the shader
ABI module.

The D3D12 library (--format d3d12) has the same header with a seventh field on each entry line, the
byte count of that entry's DXIL, and a closing `dxil` line:

  rojorhi.d3d12.shaderlibrary
  abi <N>
  entry <name> <stage> <x> <y> <z> <bytes>     one line per entry point, in reflection order
  dxil <total>
  <total raw bytes: each entry's DXIL container in entry order, then end of file>

<total> is the sum of the entries' byte counts. Each DXIL container is the signed output of the
compiler (--dxil NAME=PATH, one for each entry): it starts with `DXBC`, its size field at byte 24
equals the file's length and its digest at bytes 4 to 19 is not all zero.

The Null library (--format null) is text only:

  rojorhi.null.shaderlibrary
  abi <N>
  entry <name> <stage>                         one line per entry point, in reflection order

It has no sizes and no payload; the parser of Backends/Null/Source/NullShaderLibrary.cpp reads it.
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
D3D12_MAGIC_LINE = "rojorhi.d3d12.shaderlibrary"
NULL_MAGIC_LINE = "rojorhi.null.shaderlibrary"
DXIL_MAGIC = b"DXBC"
DXIL_DIGEST = slice(4, 20)
DXIL_SIZE_FIELD = 24
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


def package_null(abi: int, entries: list[Reflected]) -> bytes:
    lines = [NULL_MAGIC_LINE, f"abi {abi}"]
    lines += [f"entry {e.name} {e.stage}" for e in entries]
    return ("\n".join(lines) + "\n").encode("ascii")


def check_dxil(name: str, data: bytes) -> None:
    if not data.startswith(DXIL_MAGIC):
        raise LibraryError(f"the DXIL of entry '{name}' is not a DXIL container: it does not start with DXBC")
    if len(data) < DXIL_SIZE_FIELD + 4:
        raise LibraryError(f"the DXIL of entry '{name}' is truncated: {len(data)} bytes")
    declared = struct.unpack_from("<I", data, DXIL_SIZE_FIELD)[0]
    if declared != len(data):
        raise LibraryError(
            f"the DXIL of entry '{name}' declares {declared} bytes in its container but the file has {len(data)}"
        )
    if not any(data[DXIL_DIGEST]):
        raise LibraryError(f"the DXIL of entry '{name}' is unsigned: its container digest is all zero")


def package_d3d12(abi: int, entries: list[Entry], dxil: dict[str, bytes]) -> bytes:
    names = [e.name for e in entries]
    missing = [n for n in names if n not in dxil]
    if missing:
        raise LibraryError(f"no DXIL for entry '{missing[0]}'")
    extra = sorted(n for n in dxil if n not in names)
    if extra:
        raise LibraryError(f"DXIL given for '{extra[0]}', which is not an entry of the shader")
    for name in names:
        check_dxil(name, dxil[name])
    lines = [D3D12_MAGIC_LINE, f"abi {abi}"]
    lines += [
        f"entry {e.name} {e.stage} {e.size[0]} {e.size[1]} {e.size[2]} {len(dxil[e.name])}" for e in entries
    ]
    lines.append(f"dxil {sum(len(dxil[n]) for n in names)}")
    return ("\n".join(lines) + "\n").encode("ascii") + b"".join(dxil[n] for n in names)


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


def build_d3d12(module: Path, reflection: Path, spirv: Path, dxil: dict[str, Path]) -> bytes:
    try:
        abi = abi_version(read_input(module, False))
    except LibraryError as error:
        raise LibraryError(f"{module}: {error}") from error
    try:
        reflected = reflected_entries(json.loads(read_input(reflection, False)))
    except (ValueError, LibraryError) as error:
        raise LibraryError(f"{reflection}: {error}") from error
    try:
        entries = library_entries(reflected, spirv_entries(read_input(spirv, True)))
    except LibraryError as error:
        raise LibraryError(f"{spirv}: {error}") from error
    contents: dict[str, bytes] = {}
    for name, path in dxil.items():
        try:
            contents[name] = read_input(path, True)
            check_dxil(name, contents[name])
        except LibraryError as error:
            raise LibraryError(f"{path}: {error}") from error
    try:
        return package_d3d12(abi, entries, contents)
    except LibraryError as error:
        raise LibraryError(f"{spirv}: {error}") from error


def build_null(module: Path, reflection: Path) -> bytes:
    try:
        abi = abi_version(read_input(module, False))
    except LibraryError as error:
        raise LibraryError(f"{module}: {error}") from error
    try:
        reflected = reflected_entries(json.loads(read_input(reflection, False)))
    except (ValueError, LibraryError) as error:
        raise LibraryError(f"{reflection}: {error}") from error
    return package_null(abi, reflected)


def dxil_argument(text: str) -> tuple[str, Path]:
    name, separator, path = text.partition("=")
    if not separator or not NAME_PATTERN.fullmatch(name) or not path:
        raise argparse.ArgumentTypeError(f"'{text}' is not NAME=PATH")
    return name, Path(path)


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
    parser.add_argument("--spirv", type=Path, help="SPIR-V compile of the same shader (metal4, d3d12)")
    parser.add_argument("--format", choices=("metal4", "d3d12", "null"), default="metal4", help="library to write")
    parser.add_argument("--metallib", type=Path, help="metallib of the same shader (metal4)")
    parser.add_argument(
        "--dxil", action="append", default=[], type=dxil_argument, metavar="NAME=PATH",
        help="signed DXIL of the entry NAME, once for each entry (d3d12)",
    )  # fmt: skip
    parser.add_argument("--out", required=True, type=Path, help="library to write")
    args = parser.parse_args(argv)
    if args.format == "null" and (args.spirv is not None or args.metallib is not None or args.dxil):
        parser.error("--format null takes no --spirv, --metallib or --dxil")
    if args.format != "null" and args.spirv is None:
        parser.error(f"--format {args.format} takes --spirv")
    if args.format == "metal4" and (args.metallib is None or args.dxil):
        parser.error("--format metal4 takes --metallib and no --dxil")
    if args.format == "d3d12" and (args.metallib is not None or not args.dxil):
        parser.error("--format d3d12 takes --dxil NAME=PATH and no --metallib")
    try:
        if args.format == "null":
            data = build_null(args.module, args.reflection)
        elif args.format == "d3d12":
            dxil = dict(args.dxil)
            if len(dxil) != len(args.dxil):
                raise LibraryError("--dxil names an entry twice")
            data = build_d3d12(args.module, args.reflection, args.spirv, dxil)
        else:
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
