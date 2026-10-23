from __future__ import annotations

import io
import json
import re
import struct
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

from Tools import shader_library as library

ROOT = Path(__file__).resolve().parents[2]
METALLIB = b"MTLB" + bytes(range(12))

VERTEX, FRAGMENT, COMPUTE, TASK, MESH = 0, 4, 5, 5364, 5365


def op(opcode: int, *words: int) -> bytes:
    return struct.pack(f"<{len(words) + 1}I", ((len(words) + 1) << 16) | opcode, *words)


def name_words(name: str) -> list[int]:
    data = name.encode() + b"\0"
    data += b"\0" * (-len(data) % 4)
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def entry_point(model: int, function: int, name: str) -> bytes:
    return op(15, model, function, *name_words(name))


def local_size(function: int, x: int, y: int, z: int) -> bytes:
    return op(16, function, 17, x, y, z)


def module(*instructions: bytes) -> bytes:
    return struct.pack("<5I", 0x07230203, 0x10600, 0, 100, 0) + b"".join(instructions)


def reflected(name: str, stage: str, size: list[int] | None = None) -> dict:
    entry: dict = {"name": name, "stage": stage}
    if size is not None:
        entry["threadGroupSize"] = size
    return entry


def mesh_pair_spirv() -> bytes:
    return module(
        entry_point(TASK, 1, "taskMain"),
        entry_point(MESH, 2, "meshMain"),
        local_size(1, 1, 1, 1),
        local_size(2, 3, 1, 1),
    )


def mesh_pair_reflection() -> dict:
    return {"entryPoints": [reflected("taskMain", "amplification"), reflected("meshMain", "mesh")]}


def build(reflection: dict, spirv: bytes, metallib: bytes = METALLIB, abi: int = 1) -> bytes:
    entries = library.library_entries(library.reflected_entries(reflection), library.spirv_entries(spirv))
    return library.package(abi, entries, metallib)


class PackageTest(unittest.TestCase):
    def test_task_and_mesh_pair_bytes(self) -> None:
        expected = (
            b"rojorhi.metal4.shaderlibrary\nabi 1\nentry taskMain task 1 1 1\n"
            b"entry meshMain mesh 3 1 1\nmetallib 16\n" + METALLIB
        )
        self.assertEqual(build(mesh_pair_reflection(), mesh_pair_spirv()), expected)

    def test_vertex_and_fragment_pack_zero_size(self) -> None:
        spirv = module(entry_point(VERTEX, 1, "vsMain"), entry_point(FRAGMENT, 2, "fsMain"))
        reflection = {"entryPoints": [reflected("vsMain", "vertex"), reflected("fsMain", "fragment")]}
        text = build(reflection, spirv)
        self.assertIn(b"entry vsMain vertex 0 0 0\nentry fsMain fragment 0 0 0\n", text)

    def test_mesh_size_comes_from_local_size(self) -> None:
        self.assertIn(b"entry meshMain mesh 3 1 1\n", build(mesh_pair_reflection(), mesh_pair_spirv()))

    def test_amplification_packs_as_task(self) -> None:
        self.assertIn(b"entry taskMain task 1 1 1\n", build(mesh_pair_reflection(), mesh_pair_spirv()))

    def test_compute_size_matches_reflection(self) -> None:
        spirv = module(entry_point(COMPUTE, 1, "computeMain"), local_size(1, 8, 4, 2))
        good = {"entryPoints": [reflected("computeMain", "compute", [8, 4, 2])]}
        self.assertIn(b"entry computeMain compute 8 4 2\n", build(good, spirv))
        bad = {"entryPoints": [reflected("computeMain", "compute", [8, 4, 1])]}
        with self.assertRaisesRegex(library.LibraryError, r"computeMain.*8 4 2.*8 4 1"):
            build(bad, spirv)

    def test_missing_local_size_fails(self) -> None:
        for model, stage in ((COMPUTE, "compute"), (TASK, "amplification"), (MESH, "mesh")):
            spirv = module(entry_point(model, 1, "e"))
            reflection = {"entryPoints": [reflected("e", stage, [1, 1, 1] if stage == "compute" else None)]}
            with self.assertRaisesRegex(library.LibraryError, "LocalSize"):
                build(reflection, spirv)

    def test_local_size_id_fails(self) -> None:
        spirv = module(entry_point(COMPUTE, 1, "e"), op(16, 1, 38, 5, 6, 7))
        reflection = {"entryPoints": [reflected("e", "compute", [1, 1, 1])]}
        with self.assertRaisesRegex(library.LibraryError, "specialization-constant"):
            build(reflection, spirv)

    def test_zero_local_size_component_fails(self) -> None:
        spirv = module(entry_point(MESH, 1, "e"), local_size(1, 1, 0, 1))
        with self.assertRaisesRegex(library.LibraryError, "at least 1"):
            build({"entryPoints": [reflected("e", "mesh")]}, spirv)

    def test_entry_missing_from_spirv_fails(self) -> None:
        spirv = module(entry_point(VERTEX, 1, "other"))
        with self.assertRaisesRegex(library.LibraryError, "vsMain"):
            build({"entryPoints": [reflected("vsMain", "vertex")]}, spirv)

    def test_execution_model_mismatch_fails(self) -> None:
        spirv = module(entry_point(FRAGMENT, 1, "vsMain"))
        with self.assertRaisesRegex(library.LibraryError, "vsMain"):
            build({"entryPoints": [reflected("vsMain", "vertex")]}, spirv)

    def test_bad_magic_fails(self) -> None:
        data = bytearray(module(entry_point(VERTEX, 1, "e")))
        data[0] ^= 0xFF
        with self.assertRaisesRegex(library.LibraryError, "magic"):
            library.spirv_entries(bytes(data))

    def test_truncated_instruction_fails(self) -> None:
        data = module(entry_point(VERTEX, 1, "entry"))[:-4]
        with self.assertRaisesRegex(library.LibraryError, "truncated"):
            library.spirv_entries(data)

    def test_zero_word_count_fails(self) -> None:
        data = module() + struct.pack("<I", 15)
        with self.assertRaisesRegex(library.LibraryError, "word count"):
            library.spirv_entries(data)

    def test_unknown_stage_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "geometry"):
            library.reflected_entries({"entryPoints": [reflected("g", "geometry")]})

    def test_repeated_name_fails(self) -> None:
        entries = [reflected("a", "vertex"), reflected("a", "fragment")]
        with self.assertRaisesRegex(library.LibraryError, "repeated"):
            library.reflected_entries({"entryPoints": entries})

    def test_invalid_name_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "1bad"):
            library.reflected_entries({"entryPoints": [reflected("1bad", "vertex")]})

    def test_wrong_shaped_reflection_fails(self) -> None:
        for bad in (
            [],
            {"entryPoints": {}},
            {"entryPoints": ["x"]},
            {"entryPoints": [{"name": 5, "stage": "vertex"}]},
            {"entryPoints": [{"name": "a", "stage": 3}]},
            {"entryPoints": [{"name": "a", "stage": "compute", "threadGroupSize": 8}]},
        ):
            with self.assertRaises(library.LibraryError, msg=repr(bad)):
                library.reflected_entries(bad)

    def test_duplicate_spirv_entry_fails(self) -> None:
        spirv = module(entry_point(VERTEX, 1, "e"), entry_point(VERTEX, 2, "e"))
        with self.assertRaisesRegex(library.LibraryError, "more than one"):
            library.spirv_entries(spirv)

    def test_no_entries_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "no entry"):
            library.reflected_entries({"entryPoints": []})

    def test_payload_without_mtlb_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "not a metallib"):
            build(mesh_pair_reflection(), mesh_pair_spirv(), metallib=b"XXXXdata")


class AbiVersionTest(unittest.TestCase):
    DECLARATION = "public static const uint ROJORHI_SHADER_ABI_VERSION = {};\n"

    def test_reads_version(self) -> None:
        self.assertEqual(library.abi_version("module m;\n" + self.DECLARATION.format(7)), 7)

    def test_tolerates_whitespace(self) -> None:
        text = "public  static const  uint   ROJORHI_SHADER_ABI_VERSION=3 ;"
        self.assertEqual(library.abi_version(text), 3)

    def test_missing_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "ROJORHI_SHADER_ABI_VERSION"):
            library.abi_version("module m;\n")

    def test_two_declarations_fail(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "more than one"):
            library.abi_version(self.DECLARATION.format(1) + self.DECLARATION.format(2))

    def test_zero_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "at least 1"):
            library.abi_version(self.DECLARATION.format(0))

    def test_ignores_commented_declarations(self) -> None:
        text = "// " + self.DECLARATION + "/* " + self.DECLARATION + " */\n" + self.DECLARATION.format(4)
        self.assertEqual(library.abi_version(text), 4)

    def test_real_module_matches_cpp_constant(self) -> None:
        module_text = (ROOT / "Shaders/RojoRHI/RojoRHI.slang").read_text()
        header = (ROOT / "Source/Core/Pipelines.h").read_text()
        match = re.search(r"kShaderAbiVersion\s*=\s*(\d+)\s*;", header)
        self.assertIsNotNone(match)
        self.assertEqual(library.abi_version(module_text), int(match.group(1)))


class MainTest(unittest.TestCase):
    def run_main(self, directory: Path, spirv: bytes, metallib: bytes = METALLIB) -> tuple[int, str]:
        (directory / "m.slang").write_text("public static const uint ROJORHI_SHADER_ABI_VERSION = 1;\n")
        (directory / "r.json").write_text(json.dumps(mesh_pair_reflection()))
        (directory / "s.spv").write_bytes(spirv)
        (directory / "l.metallib").write_bytes(metallib)
        argv = [
            "--module", str(directory / "m.slang"),
            "--reflection", str(directory / "r.json"),
            "--spirv", str(directory / "s.spv"),
            "--metallib", str(directory / "l.metallib"),
            "--out", str(directory / "o.metal4lib"),
        ]  # fmt: skip
        err = io.StringIO()
        with redirect_stderr(err):
            code = library.main(argv)
        return code, err.getvalue()

    def test_writes_file(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            code, _ = self.run_main(Path(name), mesh_pair_spirv())
            self.assertEqual(code, 0)
            self.assertTrue((Path(name) / "o.metal4lib").read_bytes().endswith(METALLIB))
            self.assertEqual(sorted(p.name for p in Path(name).iterdir() if p.name.startswith(".")), [])

    def test_failure_removes_stale_output(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "o.metal4lib"
            out.write_bytes(b"stale")
            code, message = self.run_main(Path(name), mesh_pair_spirv(), metallib=b"nope")
            self.assertEqual(code, 1)
            self.assertIn("shader_library:", message)
            self.assertFalse(out.exists())

    def test_list_reflection_exits_one_and_removes_stale_output(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            out = directory / "o.metal4lib"
            out.write_bytes(b"stale")
            (directory / "m.slang").write_text("public static const uint ROJORHI_SHADER_ABI_VERSION = 1;\n")
            (directory / "r.json").write_text("[]")
            (directory / "s.spv").write_bytes(mesh_pair_spirv())
            (directory / "l.metallib").write_bytes(METALLIB)
            err = io.StringIO()
            with redirect_stderr(err):
                code = library.main(
                    ["--module", str(directory / "m.slang"), "--reflection", str(directory / "r.json"),
                     "--spirv", str(directory / "s.spv"), "--metallib", str(directory / "l.metallib"),
                     "--out", str(out)]
                )  # fmt: skip
            self.assertEqual(code, 1)
            self.assertIn("shader_library:", err.getvalue())
            self.assertFalse(out.exists())

    def test_unreadable_input_fails(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            err = io.StringIO()
            with redirect_stderr(err):
                code = library.main(
                    ["--module", f"{name}/none", "--reflection", "x", "--spirv", "x", "--metallib", "x",
                     "--out", f"{name}/o"]
                )  # fmt: skip
            self.assertEqual(code, 1)
            self.assertIn("shader_library:", err.getvalue())


def dxbc(payload: bytes, digest: bytes = b"\x01" * 16) -> bytes:
    return b"DXBC" + digest + b"\x01\x00\x00\x00" + struct.pack("<I", 32 + len(payload)) + b"\x00\x00\x00\x00" + payload


class D3D12PackageTest(unittest.TestCase):
    MODULE = "public static const uint ROJORHI_SHADER_ABI_VERSION = 1;\n"

    def entries(self) -> list[library.Entry]:
        return library.library_entries(
            library.reflected_entries(mesh_pair_reflection()), library.spirv_entries(mesh_pair_spirv())
        )

    def test_mesh_pair_bytes(self) -> None:
        a, b = dxbc(b"task"), dxbc(b"mesh!")
        expected = (
            b"rojorhi.d3d12.shaderlibrary\nabi 1\n"
            + f"entry taskMain task 1 1 1 {len(a)}\nentry meshMain mesh 3 1 1 {len(b)}\n".encode()
            + f"dxil {len(a) + len(b)}\n".encode()
            + a
            + b
        )
        self.assertEqual(library.package_d3d12(1, self.entries(), {"taskMain": a, "meshMain": b}), expected)

    def test_vertex_and_fragment_pack_zero_size(self) -> None:
        entries = [library.Entry("vsMain", "vertex", (0, 0, 0)), library.Entry("fsMain", "fragment", (0, 0, 0))]
        v, f = dxbc(b"v"), dxbc(b"ff")
        data = library.package_d3d12(1, entries, {"vsMain": v, "fsMain": f})
        self.assertIn(f"entry vsMain vertex 0 0 0 {len(v)}\n".encode(), data)
        self.assertIn(f"entry fsMain fragment 0 0 0 {len(f)}\n".encode(), data)

    def test_missing_dxil_for_an_entry_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "meshMain"):
            library.package_d3d12(1, self.entries(), {"taskMain": dxbc(b"t")})

    def test_extra_dxil_fails(self) -> None:
        both = {"taskMain": dxbc(b"t"), "meshMain": dxbc(b"m"), "stray": dxbc(b"s")}
        with self.assertRaisesRegex(library.LibraryError, "stray"):
            library.package_d3d12(1, self.entries(), both)

    def test_dxil_without_dxbc_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "taskMain"):
            library.check_dxil("taskMain", b"MTLB" + bytes(40))

    def test_dxil_size_field_mismatch_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "meshMain"):
            library.check_dxil("meshMain", dxbc(b"abc") + b"x")

    def test_unsigned_dxil_fails(self) -> None:
        with self.assertRaisesRegex(library.LibraryError, "unsigned"):
            library.check_dxil("taskMain", dxbc(b"abc", digest=bytes(16)))

    def test_main_d3d12_writes_the_library(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            d = Path(name)
            (d / "m.slang").write_text(self.MODULE)
            (d / "r.json").write_text(json.dumps(mesh_pair_reflection()))
            (d / "s.spv").write_bytes(mesh_pair_spirv())
            a, b = dxbc(b"task"), dxbc(b"mesh")
            (d / "a.dxil").write_bytes(a)
            (d / "b.dxil").write_bytes(b)
            argv = [
                "--format", "d3d12", "--module", str(d / "m.slang"), "--reflection", str(d / "r.json"),
                "--spirv", str(d / "s.spv"), "--dxil", f"taskMain={d / 'a.dxil'}",
                "--dxil", f"meshMain={d / 'b.dxil'}", "--out", str(d / "o.d3d12lib"),
            ]  # fmt: skip
            self.assertEqual(library.main(argv), 0)
            data = (d / "o.d3d12lib").read_bytes()
            self.assertTrue(data.startswith(b"rojorhi.d3d12.shaderlibrary\nabi 1\n"))
            self.assertTrue(data.endswith(a + b))

    def test_main_d3d12_failure_names_the_file_and_removes_output(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            d = Path(name)
            (d / "m.slang").write_text(self.MODULE)
            (d / "r.json").write_text(json.dumps(mesh_pair_reflection()))
            (d / "s.spv").write_bytes(mesh_pair_spirv())
            (d / "a.dxil").write_bytes(dxbc(b"t", digest=bytes(16)))
            out = d / "o.d3d12lib"
            out.write_bytes(b"stale")
            argv = [
                "--format", "d3d12", "--module", str(d / "m.slang"), "--reflection", str(d / "r.json"),
                "--spirv", str(d / "s.spv"), "--dxil", f"taskMain={d / 'a.dxil'}",
                "--dxil", f"meshMain={d / 'a.dxil'}", "--out", str(out),
            ]  # fmt: skip
            err = io.StringIO()
            with redirect_stderr(err):
                self.assertEqual(library.main(argv), 1)
            self.assertIn("a.dxil", err.getvalue())
            self.assertFalse(out.exists())

    def test_main_metal4_unchanged_without_format(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            d = Path(name)
            (d / "m.slang").write_text(self.MODULE)
            (d / "r.json").write_text(json.dumps(mesh_pair_reflection()))
            (d / "s.spv").write_bytes(mesh_pair_spirv())
            (d / "l.metallib").write_bytes(METALLIB)
            argv = [
                "--module", str(d / "m.slang"), "--reflection", str(d / "r.json"), "--spirv", str(d / "s.spv"),
                "--metallib", str(d / "l.metallib"), "--out", str(d / "o.metal4lib"),
            ]  # fmt: skip
            self.assertEqual(library.main(argv), 0)
            self.assertTrue((d / "o.metal4lib").read_bytes().startswith(b"rojorhi.metal4.shaderlibrary\n"))

    def test_main_requires_the_inputs_of_its_format(self) -> None:
        err = io.StringIO()
        with redirect_stderr(err), self.assertRaises(SystemExit):
            library.main(["--format", "d3d12", "--module", "m", "--reflection", "r", "--spirv", "s", "--out", "o"])


class NullLibraryTests(unittest.TestCase):
    MODULE = "public static const uint ROJORHI_SHADER_ABI_VERSION = 2;\n"

    def inputs(self, directory: Path, reflection: object) -> list[str]:
        (directory / "m.slang").write_text(self.MODULE)
        (directory / "r.json").write_text(json.dumps(reflection))
        return ["--format", "null", "--module", str(directory / "m.slang"),
                "--reflection", str(directory / "r.json"), "--out", str(directory / "o.nulllib")]  # fmt: skip

    def test_null_library_lists_reflection_entries(self) -> None:
        entries = [library.Reflected("vsMain", "vertex", None), library.Reflected("fsMain", "fragment", None)]
        self.assertEqual(
            library.package_null(2, entries),
            b"rojorhi.null.shaderlibrary\nabi 2\nentry vsMain vertex\nentry fsMain fragment\n",
        )

    def test_amplification_is_task(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            argv = self.inputs(directory, mesh_pair_reflection())
            self.assertEqual(library.main(argv), 0)
            self.assertEqual(
                (directory / "o.nulllib").read_bytes(),
                b"rojorhi.null.shaderlibrary\nabi 2\nentry taskMain task\nentry meshMain mesh\n",
            )

    def test_null_needs_no_spirv(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            argv = self.inputs(directory, {"entryPoints": [reflected("csMain", "compute", [8, 1, 1])]})
            self.assertEqual(library.main(argv), 0)
            self.assertTrue((directory / "o.nulllib").is_file())

    def test_null_refuses_metallib_spirv_and_dxil(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            argv = self.inputs(Path(name), mesh_pair_reflection())
            for extra in (["--metallib", "x"], ["--spirv", "x"], ["--dxil", "a=b"]):
                with self.subTest(extra=extra), redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as raised:
                        library.main(argv + extra)
                    self.assertEqual(raised.exception.code, 2)

    def test_null_bad_reflection_fails_and_removes_out(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            argv = self.inputs(directory, [])
            (directory / "o.nulllib").write_bytes(b"stale")
            err = io.StringIO()
            with redirect_stderr(err):
                code = library.main(argv)
            self.assertEqual(code, 1)
            self.assertIn(f"{directory / 'r.json'}:", err.getvalue())
            self.assertFalse((directory / "o.nulllib").exists())

    def test_spirv_is_required_for_metal4_and_d3d12(self) -> None:
        for fmt in ("metal4", "d3d12"):
            with self.subTest(format=fmt), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    library.main(["--format", fmt, "--module", "m", "--reflection", "r", "--out", "o"])
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
