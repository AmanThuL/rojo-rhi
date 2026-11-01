#!/usr/bin/env python3
"""Stands in for slangc and xcrun in the rojorhi.slang fixtures: records each call and writes outputs the packaging
tool accepts. MOCK_LOG names the call log; MOCK_FAIL, when set, names a `<source stem>:<target>` compile that fails."""
from __future__ import annotations

import json
import os
import re
import struct
import sys
from pathlib import Path


def instruction(opcode: int, *operands: int) -> bytes:
    return struct.pack(f"<{len(operands) + 1}I", ((len(operands) + 1) << 16) | opcode, *operands)


def literal(text: str) -> tuple[int, ...]:
    data = text.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return struct.unpack(f"<{len(data) // 4}I", data)


def main() -> None:
    tool = Path(sys.argv[0]).name
    args = sys.argv[1:]
    with open(os.environ["MOCK_LOG"], "a") as log:
        log.write(json.dumps({"tool": tool, "argv": args}) + "\n")
    if tool == "xcrun" and args[:2] == ["-f", "metal"]:
        return
    if args == ["-version"]:
        print("mock 1.0", file=sys.stderr)
        return
    if tool == "xcrun":
        source, target = Path(args[-1]), "metallib"
    else:
        source, target = Path(args[0]), args[args.index("-target") + 1]
    if os.environ.get("MOCK_FAIL") == f"{source.name.split('.')[0]}:{target}":
        sys.exit(f"mock {target} failure")
    if tool == "xcrun":
        payload = b"MTLB" + source.read_bytes()
    else:
        entries = re.findall(r"void\s+(\w+)\s*\(", source.read_text())
        if target == "metal":
            reflection = {"entryPoints": [{"name": name, "stage": "compute", "threadGroupSize": [1, 1, 1]}
                                          for name in entries]}
            Path(args[args.index("-reflection-json") + 1]).write_text(json.dumps(reflection))
            payload = b"// msl\n"
        elif target == "spirv":
            payload = struct.pack("<5I", 0x07230203, 0x10600, 0, 100, 0)
            for index, name in enumerate(entries, 1):
                payload += instruction(15, 5, index, *literal(name)) + instruction(16, index, 17, 1, 1, 1)
        elif target == "dxil":
            payload = b"DXBC" + bytes(range(1, 17)) + bytes(4) + struct.pack("<I", 28)
        else:
            print("// hlsl")
            return
    Path(args[args.index("-o") + 1]).write_bytes(payload)


if __name__ == "__main__":
    main()
