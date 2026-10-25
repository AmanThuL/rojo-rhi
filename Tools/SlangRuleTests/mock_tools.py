#!/usr/bin/env python3
"""Executable stand-ins: record argv and emit packaging-valid, non-executable payloads."""
from __future__ import annotations

import json
import os
import re
import struct
import sys
from pathlib import Path


def op(code, *words):
    return struct.pack(f"<{len(words) + 1}I", ((len(words) + 1) << 16) | code, *words)


def words(text):
    data = text.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return struct.unpack(f"<{len(data) // 4}I", data)


def main():
    args = sys.argv[1:]
    tool = Path(sys.argv[0]).name
    with open(os.environ["MOCK_LOG"], "a") as stream:
        stream.write(json.dumps({"tool": tool, "argv": args}) + "\n")
    control = json.loads(Path(os.environ["MOCK_CONTROL"]).read_text())
    if tool == "xcrun" and args[:2] == ["-f", "metal"]:
        sys.exit(1 if control.get("no_metal") else 0)
    if tool == "xcrun":
        source = Path(args[-1])
        stage = "metallib"
    else:
        source = Path(args[0])
        stage = args[args.index("-target") + 1]
    if source.stem == "DxilProbe" and control.get("no_dxil"):
        sys.exit("error[E00100]: failed to load downstream compiler 'dxc'")
    if source.stem == "Selected" and control.get("fail") == stage:
        sys.exit("mock selected " + stage + " diagnostic")
    marker = json.dumps(args, separators=(",", ":"))
    if tool == "xcrun":
        payload = b"MTLB" + source.read_bytes() + marker.encode()
    else:
        entries = re.findall(r"void\s+(\w+)\s*\(", source.read_text())
        if stage == "metal":
            reflection = {"entryPoints": [{"name": n, "stage": "compute", "threadGroupSize": [8, 4, 2]}
                                            for n in entries], "mockPolicy": marker}
            Path(args[args.index("-reflection-json") + 1]).write_text(json.dumps(reflection))
            payload = ("// " + marker + "\n").encode()
        elif stage == "spirv":
            payload = struct.pack("<5I", 0x07230203, 0x10600, 0, 100, 0)
            for index, name in enumerate(entries, 1):
                payload += op(15, 5, index, *words(name)) + op(16, index, 17, 8, 4, 2)
            payload += op(7, 99, *words(marker))
        elif stage == "dxil":
            payload = b"DXBC" + bytes(range(1, 17)) + bytes(4)
            payload += struct.pack("<I", 28 + len(marker.encode())) + marker.encode()
        else:
            print("// " + marker)
            return
    Path(args[args.index("-o") + 1]).write_bytes(payload)


if __name__ == "__main__":
    main()
