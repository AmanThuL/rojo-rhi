from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import check_dxil


# Lines cut from `dxc -dumpbin` of the compiled AbiNonUniform and AbiRawAccess kernels.
ROOT_HANDLE = """\
  %1 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind zeroinitializer, i32 0, i1 false)
  %3 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %1, %dx.types.ResourceProperties { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer
"""

TEXTURE_KERNEL = ROOT_HANDLE + """\
declare %dx.types.Handle @dx.op.createHandleFromHeap(i32, i32, i1, i1) #1
  %4 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %3, i32 16, i32 undef, i8 3, i32 4)
  %14 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %13, i1 false, i1 true)
  %15 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %14, %dx.types.ResourceProperties { i32 2, i32 1033 })  ; AnnotateHandle(res,props)  resource: Texture2D<4xF32>
  %22 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %21, i1 false, i1 true)
  %23 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %22, %dx.types.ResourceProperties { i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: RWByteAddressBuffer
  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %23, i32 %25, i32 undef, float %17, float %18, float %19, float %20, i8 15, i32 4)
"""

UNFLAGGED_STORE_KERNEL = ROOT_HANDLE + """\
  %4 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %3, i32 0, i32 undef, i8 3, i32 4)
  %15 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %14, i1 false, i1 false)
  %16 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %15, %dx.types.ResourceProperties { i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: RWByteAddressBuffer
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %16, i32 %18, i32 undef, i32 %2, i32 undef, i32 undef, i32 undef, i8 1, i32 4)
"""

LOAD_KERNEL = ROOT_HANDLE + """\
  %17 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %15, i1 false, i1 true)
  %18 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %17, %dx.types.ResourceProperties { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer
  %21 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %18, i32 %20, i32 undef, i8 15, i32 4)
"""

SAMPLER_FROM_RESOURCE_HEAP = ROOT_HANDLE + """\
  %16 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %15, i1 false, i1 true)
  %17 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %16, %dx.types.ResourceProperties { i32 14, i32 0 })  ; AnnotateHandle(res,props)  resource: SamplerState
"""

RAW_ROOT_LOADS = ROOT_HANDLE + """\
  %4 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %3, i32 0, i32 undef, i8 3, i32 4)
  %7 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %3, i32 8, i32 undef, i8 3, i32 4)
  %10 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %5, i1 false, i1 false)
  %11 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %10, %dx.types.ResourceProperties { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer
"""

FULL_WIDTH_RAW = RAW_ROOT_LOADS + """\
  %14 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %11, i32 %13, i32 undef, i8 15, i32 4)
  %24 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %23, i1 false, i1 true)
  %25 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %24, %dx.types.ResourceProperties { i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: RWByteAddressBuffer
  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %25, i32 %26, i32 undef, i32 %19, i32 %20, i32 %21, i32 %22, i8 15, i32 4)
"""

SCALAR_RAW = RAW_ROOT_LOADS + "".join(
    f"  %{n} = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %11, i32 %{n - 1},"
    " i32 undef, i8 1, i32 4)\n" for n in (14, 17, 20, 23)
) + """\
  %30 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %29, i1 false, i1 false)
  %31 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %30, %dx.types.ResourceProperties { i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: RWByteAddressBuffer
""" + "".join(
    f"  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %31, i32 %{n}, i32 undef, i32 %{n - 7},"
    " i32 undef, i32 undef, i32 undef, i8 1, i32 4)\n" for n in (32, 33, 34, 35)
)

THIRD_ROOT_LOAD = (
    "  %8 = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %3, i32 16, i32 undef,"
    " i8 15, i32 4)\n"
)


class NonUniformTests(unittest.TestCase):
    def test_flagged_heap_handles_of_the_kind_pass(self) -> None:
        self.assertEqual(check_dxil.nonuniform_errors("t.dxil", TEXTURE_KERNEL, "Texture2D"), [])

    def test_a_heap_handle_without_the_flag_is_reported(self) -> None:
        errors = check_dxil.nonuniform_errors("s.dxil", UNFLAGGED_STORE_KERNEL, "RWByteAddressBuffer")
        self.assertEqual(len(errors), 1)
        self.assertIn("RWByteAddressBuffer", errors[0])

    def test_a_kernel_without_its_kind_is_reported(self) -> None:
        errors = check_dxil.nonuniform_errors("l.dxil", LOAD_KERNEL, "Texture2D")
        self.assertEqual(len(errors), 1)
        self.assertIn("no longer fetches its kind", errors[0])

    def test_a_sampler_must_come_from_the_sampler_heap(self) -> None:
        errors = check_dxil.nonuniform_errors("p.dxil", SAMPLER_FROM_RESOURCE_HEAP, "SamplerState")
        self.assertEqual(len(errors), 1)
        self.assertIn("SamplerState", errors[0])

    def test_crlf_disassembly_parses(self) -> None:
        crlf = TEXTURE_KERNEL.replace("\n", "\r\n")
        self.assertEqual(check_dxil.nonuniform_errors("t.dxil", crlf, "Texture2D"), [])
        self.assertEqual(check_dxil.parse_handles(crlf)["%15"].resource, "Texture2D")


class RawAccessTests(unittest.TestCase):
    def test_full_width_accesses_pass(self) -> None:
        self.assertEqual(check_dxil.raw_access_errors("r.dxil", FULL_WIDTH_RAW), [])

    def test_scalar_accesses_are_reported(self) -> None:
        errors = check_dxil.raw_access_errors("r.dxil", SCALAR_RAW)
        self.assertEqual(len(errors), 2)
        self.assertIn("load", errors[0])
        self.assertIn("store", errors[1])

    def test_extra_root_loads_are_reported(self) -> None:
        errors = check_dxil.raw_access_errors("r.dxil", FULL_WIDTH_RAW + THIRD_ROOT_LOAD)
        self.assertEqual(len(errors), 1)
        self.assertIn("root", errors[0])

    def test_a_local_array_is_reported(self) -> None:
        errors = check_dxil.raw_access_errors("r.dxil", FULL_WIDTH_RAW + "  %2 = alloca [4 x i32], align 4\n")
        self.assertEqual(len(errors), 1)
        self.assertIn("alloca", errors[0])


class MainTests(unittest.TestCase):
    def run_main(self, argv: list[str]) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = check_dxil.main(argv)
        return code, output.getvalue()

    def test_missing_dxil_fails_without_running_dxc(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(check_dxil, "disassemble") as disassemble:
            code, output = self.run_main(["--shaders", tmp, "--dxc", "/nonexistent"])
        self.assertEqual(code, 1)
        self.assertIn("is missing", output)
        disassemble.assert_not_called()

    def test_no_shader_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(check_dxil.find_shader_dirs(Path(tmp)), [])
            with mock.patch.object(check_dxil, "ROOT", Path(tmp)):
                code, _ = self.run_main([])
        self.assertEqual(code, 1)

    def test_no_dxc_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name in [*check_dxil.NONUNIFORM, check_dxil.RAW_ACCESS]:
                (Path(tmp) / name).write_bytes(b"")
            with mock.patch.object(check_dxil, "find_dxc", return_value=None), \
                    mock.patch.object(check_dxil, "disassemble") as disassemble:
                code, output = self.run_main(["--shaders", tmp])
        self.assertEqual(code, 1)
        self.assertIn("--dxc", output)
        disassemble.assert_not_called()

    def test_a_dxc_that_cannot_run_fails_with_one_line(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name in [*check_dxil.NONUNIFORM, check_dxil.RAW_ACCESS]:
                (Path(tmp) / name).write_bytes(b"")
            code, output = self.run_main(["--shaders", tmp, "--dxc", str(Path(tmp) / "no-dxc")])
        self.assertEqual(code, 1)
        self.assertEqual(len(output.strip().splitlines()), 1)
        self.assertIn("no-dxc", output)

    def test_a_failed_dumpbin_fails_naming_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name in [*check_dxil.NONUNIFORM, check_dxil.RAW_ACCESS]:
                (Path(tmp) / name).write_bytes(b"")
            failed = mock.Mock(returncode=1, stdout="", stderr="invalid container")
            with mock.patch.object(check_dxil.subprocess, "run", return_value=failed):
                code, output = self.run_main(["--shaders", tmp, "--dxc", "dxc"])
        self.assertEqual(code, 1)
        self.assertIn("AbiNonUniform.loadMain.dxil", output)
        self.assertIn("invalid container", output)


if __name__ == "__main__":
    unittest.main()
