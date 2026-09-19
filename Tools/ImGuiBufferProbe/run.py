#!/usr/bin/env python3
"""Build/run the Metal 4 ImGui buffer regression without changing Tests dependencies."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile


# RojoRHI/Tools/ImGuiBufferProbe/run.py, so the host repository root is one level above the
# component directory (RojoRHI/); parents[3] counts up from this file to reach it.
ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=ROOT / "build/macosx/arm64/release/libImGui.a")
    parser.add_argument(
        "--old-policy-control", action="store_true",
        help="Also require the original early-return policy to fail",
    )
    args = parser.parse_args()
    backend = ROOT / "ThirdParty/imgui/backends/imgui_impl_metal4.mm"
    if not backend.is_file():
        parser.error(f"{backend} not found; the probe needs a host checkout providing ThirdParty/imgui")
    if not args.archive.is_file():
        parser.error(f"{args.archive} not found; the probe needs a host checkout with a built libImGui.a")
    with tempfile.TemporaryDirectory(prefix="lmx-imgui-buffer-") as directory:
        output = Path(directory)

        def compile_run(source: Path, name: str) -> subprocess.CompletedProcess:
            binary = output / name
            command = [
                "xcrun", "clang++", "-std=c++23", "-fno-objc-arc", "-DIMGUI_IMPL_METAL_CPP",
                "-mmacosx-version-min=" + ".".join(platform.mac_ver()[0].split(".")[:2]),
                f'-DROJORHI_IMGUI_BACKEND_SOURCE="{source}"',
                "-I", str(ROOT / "ThirdParty/imgui"), "-I", str(ROOT / "ThirdParty/imgui/backends"),
                "-I", str(ROOT / "ThirdParty/metal-cpp"), str(Path(__file__).with_name("main.mm")),
                str(args.archive.resolve()), "-framework", "Metal", "-framework", "QuartzCore",
                "-framework", "Cocoa", "-o", str(binary),
            ]
            print(f"Building {name} from {source}", flush=True)
            print(shlex.join(command), flush=True)
            subprocess.run(command, check=True)
            result = subprocess.run(
                [str(binary)], env={**os.environ, "MTL_DEBUG_LAYER": "1"}, timeout=60,
                capture_output=True, text=True,
            )
            print(result.stdout + result.stderr, end="", flush=True)
            return result

        if compile_run(backend, "current").returncode != 0:
            return 1
        if args.old_policy_control:
            text = backend.read_text()
            needle = "NSMutableArray<MetalBuffer*>* used = sharedMetalContext.usedBuffers[sharedMetalContext.currentFrameSlot];"
            if text.count(needle) != 1:
                raise RuntimeError("Backend changed: review the original-policy mutation before running")
            old = output / "imgui_impl_metal4_old_policy.mm"
            old.write_text(text.replace(needle, needle.replace(".usedBuffers[", ".bufferCaches[")))
            result = compile_run(old, "old-policy")
            if result.returncode != 1:
                raise RuntimeError(f"Expected invariant failure (exit 1) for old policy, got {result.returncode}")
            for expected in (
                "FAIL: second viewport must not overwrite first viewport vertex upload",
                "FAIL: second viewport must not overwrite first viewport index upload",
                "FAIL: retired render target preserves its own geometry/color",
            ):
                if expected not in result.stderr:
                    raise RuntimeError(f"Original-policy control did not expose: {expected}")
            print("PASS: old early-return policy fails the same real-backend probe", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
