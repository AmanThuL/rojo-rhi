from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import check_cpp_comments as comments


def source(name: str, brief: str = "Declares an example API.") -> str:
    return "\n".join(
        [
            comments.FILE_RULER,
            f"/// @file {name}",
            f"/// @brief {brief}",
            comments.FILE_RULER,
            "",
        ]
    )


class CppCommentTests(unittest.TestCase):
    def test_valid_file_header_envelope(self) -> None:
        text = source("Example.h") + "#pragma once\n"
        self.assertEqual(comments.check_file_header(Path("Include/rojoRHI/Example.h"), text), [])
        self.assertEqual(len(comments.FILE_RULER), 120)

    def test_file_header_starts_on_first_line_and_matches_basename(self) -> None:
        text = "\n" + source("Other.h")
        errors = comments.check_file_header(Path("Include/rojoRHI/Example.h"), text)
        self.assertTrue(any(":1:" in error for error in errors))
        self.assertTrue(any("basename" in error or "expected" in error for error in errors))

    def test_file_header_requires_nonempty_brief_and_closing_ruler(self) -> None:
        text = "\n".join(
            [comments.FILE_RULER, "/// @file Example.cpp", "/// @brief ", "//----"]
        )
        errors = comments.check_file_header(Path("Source/Example.cpp"), text)
        self.assertTrue(any("non-empty" in error for error in errors))
        self.assertTrue(any("closing" in error for error in errors))

    def test_owned_roots_include_every_source_root_but_not_tests(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in (
                "Include/rojoRHI/RHI.h",
                "Backends/Metal4/Source/Private.h",
                "Source/Validate.h",
                # Test sources carry no file envelope; they lie outside every source root.
                "Tests/RHITest.cpp",
                "Tests/RHITestSupport.h",
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in comments.project_cpp_files(root)],
                ["Backends/Metal4/Source/Private.h", "Include/rojoRHI/RHI.h", "Source/Validate.h"],
            )

    def test_public_header_roots_exclude_backend_and_implementation_headers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in (
                "Include/rojoRHI/RHI.h",
                "Backends/Metal4/ImGui/Include/rojoRHI/Metal4/Metal4ImGui.h",
                "Backends/Metal4/Source/Private.h",
                "Source/Validate.h",
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in comments.public_header_files(root)],
                [
                    "Backends/Metal4/ImGui/Include/rojoRHI/Metal4/Metal4ImGui.h",
                    "Include/rojoRHI/RHI.h",
                ],
            )

    def adapter_root(self, directory: str) -> Path:
        root = Path(directory)
        for relative in (
            "Include/rojoRHI/RHI.h",
            "Backends/Metal4/ImGui/Include/rojoRHI/Metal4/Metal4ImGui.h",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        return root

    def test_adapter_headers_are_skipped_with_a_notice_when_the_database_lacks_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.adapter_root(directory)
            entries = [{"directory": str(root), "file": "Source/Validate.cpp"}]
            headers, notices = comments.api_headers(root, entries)
            self.assertEqual([p.relative_to(root).as_posix() for p in headers], ["Include/rojoRHI/RHI.h"])
            self.assertEqual(len(notices), 1)
            self.assertIn("Backends/Metal4/ImGui", notices[0])

    def test_adapter_headers_are_checked_when_the_database_has_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.adapter_root(directory)
            entries = [
                {"directory": str(root.parent), "file": f"{root.name}/Source/Validate.cpp"},
                {"directory": str(root), "file": "Backends/Metal4/ImGui/Source/Metal4ImGui.cpp"},
            ]
            headers, notices = comments.api_headers(root, entries)
            self.assertEqual(len(headers), 2)
            self.assertEqual(notices, [])

    def test_a_database_without_component_entries_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.adapter_root(directory)
            entries = [{"directory": str(root.parent), "file": "Source/Engine/Main.cpp"}]
            with self.assertRaisesRegex(RuntimeError, "no entry under the component"):
                comments.component_entries(entries, root)

    def test_header_entry_prefers_a_matching_stem_from_any_entry(self) -> None:
        root = Path("/component")
        entries = [
            {"file": "Tests/RHIResultTests.cpp"},
            {"file": "Source/Validate.cpp"},
            {"file": "Backends/Metal4/Source/Metal4Device.cpp"},
        ]
        self.assertEqual(
            comments._entry_for_header(root / "Include/rojoRHI/Validate.h", entries, root),
            {"file": "Source/Validate.cpp"},
        )
        self.assertEqual(
            comments._entry_for_header(root / "Include/rojoRHI/Heap.h", entries, root),
            {"file": "Tests/RHIResultTests.cpp"},
        )

    def test_an_empty_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            argv = ["check_cpp_comments.py", "--root", directory, "--public-api-docs", "off"]
            with mock.patch.object(sys, "argv", argv), mock.patch("sys.stderr"):
                self.assertNotEqual(comments.main(), 0)

    def test_ast_gate_observes_access_and_documentation(self) -> None:
        text = source("Example.h") + "class Example {};\n"
        ast = [{
            "kind": "NamespaceDecl",
            "loc": {"file": "Include/rojoRHI/Example.h", "line": 5, "offset": 150},
            "inner": [{
                "kind": "CXXRecordDecl", "name": "Example", "tagUsed": "class",
                "completeDefinition": True, "loc": {"line": 5, "offset": 151},
                "inner": [
                    {"kind": "AccessSpecDecl", "access": "public"},
                    {"kind": "CXXMethodDecl", "name": "missing", "loc": {"line": 7}},
                    {"kind": "CXXMethodDecl", "name": "documented", "loc": {"line": 8},
                     "inner": [{"kind": "FullComment"}]},
                    {"kind": "CXXConstructorDecl", "name": "Example", "loc": {"line": 9},
                     "explicitlyDefaulted": "default"},
                    {"kind": "AccessSpecDecl", "access": "private"},
                    {"kind": "FieldDecl", "name": "implementation", "loc": {"line": 11}},
                ],
            }],
        }]
        findings = comments.public_api_findings_from_ast(Path("Include/rojoRHI/Example.h"), text, ast)
        self.assertEqual(len(findings), 2)
        self.assertTrue(any(":5:" in finding for finding in findings))
        self.assertTrue(any(":7:" in finding for finding in findings))

    def test_ast_gate_ignores_forward_declarations_and_checks_enum_values(self) -> None:
        ast = [{
            "kind": "NamespaceDecl", "loc": {"file": "Include/rojoRHI/Example.h", "line": 5},
            "inner": [
                {"kind": "CXXRecordDecl", "name": "Forward", "loc": {"line": 6}},
                {"kind": "EnumDecl", "name": "Mode", "loc": {"line": 8}, "inner": [
                    {"kind": "FullComment"},
                    {"kind": "EnumConstantDecl", "name": "Fast", "loc": {"line": 9},
                     "inner": [{"kind": "FullComment"}]},
                    {"kind": "EnumConstantDecl", "name": "Safe", "loc": {"line": 10}},
                ]},
            ],
        }]
        findings = comments.public_api_findings_from_ast(
            Path("Include/rojoRHI/Example.h"), source("Example.h"), ast
        )
        self.assertEqual(
            findings,
            ["Include/rojoRHI/Example.h:10: public API declaration has no Doxygen comment"],
        )


if __name__ == "__main__":
    unittest.main()
