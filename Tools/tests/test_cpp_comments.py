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

    def test_adapter_sources_are_owned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "Adapters/ImGui/Source/ImGuiAdapter.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("", encoding="utf-8")
            self.assertEqual(
                [p.relative_to(root).as_posix() for p in comments.project_cpp_files(root)],
                ["Adapters/ImGui/Source/ImGuiAdapter.cpp"],
            )

    def test_benchmark_sources_are_owned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("Benchmarks/Main.cpp", "Benchmarks/Host/HostClock.h"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in comments.project_cpp_files(root)],
                ["Benchmarks/Host/HostClock.h", "Benchmarks/Main.cpp"],
            )

    def test_public_header_roots_exclude_backend_and_implementation_headers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in (
                "Include/rojoRHI/RHI.h",
                f"{comments.ADAPTER_DIRS[0]}/Include/rojoRHI/Metal4/Metal4ImGui.h",
                "Legacy/Include/rojoRHI/Legacy.h",
                "Backends/Metal4/Source/Private.h",
                "Source/Validate.h",
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in comments.public_header_files(root)],
                [
                    "Include/rojoRHI/RHI.h",
                    f"{comments.ADAPTER_DIRS[0]}/Include/rojoRHI/Metal4/Metal4ImGui.h",
                    "Legacy/Include/rojoRHI/Legacy.h",
                ],
            )

    def adapter_root(self, directory: str) -> Path:
        root = Path(directory)
        for relative in (
            "Include/rojoRHI/RHI.h",
            f"{comments.ADAPTER_DIRS[0]}/Include/rojoRHI/Metal4/Metal4ImGui.h",
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
            self.assertIn(comments.ADAPTER_DIRS[0], notices[0])

    def test_adapter_headers_are_checked_when_the_database_has_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.adapter_root(directory)
            entries = [
                {"directory": str(root.parent), "file": f"{root.name}/Source/Validate.cpp"},
                {"directory": str(root), "file": f"{comments.ADAPTER_DIRS[0]}/Source/Metal4ImGui.cpp"},
            ]
            headers, notices = comments.api_headers(root, entries)
            self.assertEqual(len(headers), 2)
            self.assertEqual(notices, [])

    def test_new_adapter_headers_are_skipped_with_a_notice_when_the_database_lacks_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("Include/rojoRHI/RHI.h", "Adapters/ImGui/Include/rojoRHI/ImGuiAdapter.h"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            entries = [{"directory": str(root), "file": "Source/Validate.cpp"}]
            headers, notices = comments.api_headers(root, entries)
            self.assertEqual([p.relative_to(root).as_posix() for p in headers], ["Include/rojoRHI/RHI.h"])
            self.assertEqual(len(notices), 1)
            self.assertIn("Adapters/ImGui", notices[0])

    def test_new_adapter_headers_are_checked_when_the_database_has_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("Include/rojoRHI/RHI.h", "Adapters/ImGui/Include/rojoRHI/ImGuiAdapter.h"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            entries = [
                {"directory": str(root), "file": "Source/Validate.cpp"},
                {"directory": str(root), "file": "Adapters/ImGui/Source/ImGuiAdapter.cpp"},
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

    def test_header_entry_prefers_a_matching_stem_within_its_tree(self) -> None:
        root = Path("/component")
        entries = [
            {"directory": str(root), "file": "Tests/RHIResultTests.cpp"},
            {"directory": str(root), "file": "Source/Validate.cpp"},
            {"directory": str(root), "file": "Backends/Metal4/Source/Metal4Device.cpp"},
        ]
        self.assertEqual(
            comments._entry_for_header(root / "Include/rojoRHI/Validate.h", entries, root),
            entries[1],
        )
        self.assertEqual(
            comments._entry_for_header(root / "Include/rojoRHI/Heap.h", entries, root),
            entries[0],
        )

    def test_header_entry_comes_from_the_headers_own_tree(self) -> None:
        root = Path("/component")
        entries = [
            {"directory": str(root), "file": "Tests/VersionTests.cpp"},
            {"directory": str(root), "file": "Legacy/Source/Validate.cpp"},
        ]
        self.assertEqual(
            comments._entry_for_header(root / "Legacy/Include/rojoRHI/Device.h", entries, root),
            entries[1],
        )
        self.assertEqual(
            comments._entry_for_header(root / "Include/rojoRHI/RHI.h", entries, root),
            entries[0],
        )

    def test_header_whose_tree_has_no_entry_fails(self) -> None:
        root = Path("/component")
        entries = [{"directory": str(root), "file": "Tests/VersionTests.cpp"}]
        with self.assertRaisesRegex(RuntimeError, "Device.h"):
            comments._entry_for_header(root / "Legacy/Include/rojoRHI/Device.h", entries, root)

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

    def test_minimal_header_rejects_a_valid_usage_identifier_in_a_comment(self) -> None:
        text = source("RHI.h") + "    void end(); ///< The list is recording (VU-rojo-cmd-0002).\n"
        errors = comments.check_minimal_comments(Path("Include/rojoRHI/RHI.h"), text)
        self.assertEqual(len(errors), 1)
        self.assertIn("RHI.h:5:", errors[0])
        self.assertIn("VU-rojo-", errors[0])

    def test_minimal_header_rejects_document_paths_adrs_and_links(self) -> None:
        for comment in (
            "// See docs/spec/07-command-recording.md.",
            "// ADR 0020 allows the lock.",
            "/* https://example.com */",
        ):
            with self.subTest(comment=comment):
                text = source("RHI.h") + comment + "\n"
                self.assertEqual(len(comments.check_minimal_comments(Path("Include/rojoRHI/RHI.h"), text)), 1)

    def test_minimal_header_accepts_short_notes_and_ignores_string_literals(self) -> None:
        text = source("RHI.h") + "\n".join(
            [
                "// Frame thread class.",
                "void beginFrame();",
                'static_assert(true, "see docs/ and http");',
                "uint64_t bits = 0; ///< Opaque.",
                "",
            ]
        )
        self.assertEqual(comments.check_minimal_comments(Path("Include/rojoRHI/RHI.h"), text), [])

    def test_only_the_v2_public_tree_follows_the_minimal_style(self) -> None:
        self.assertTrue(comments.follows_minimal_style(Path("Include/rojoRHI/RHI.h")))
        self.assertFalse(comments.follows_minimal_style(Path("Legacy/Include/rojoRHI/Device.h")))
        self.assertFalse(comments.follows_minimal_style(Path("Source/Core/DeviceCore.h")))

    def test_undocumented_declaration_passes_in_the_public_header_but_not_in_legacy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("Include/rojoRHI/RHI.h", "Legacy/Include/rojoRHI/Device.h"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source(path.name) + "void undocumented();\n", encoding="utf-8")
            database = root / "compile_commands.json"
            database.write_text(
                '[{"directory": "%s", "file": "Tests/A.cpp"}, {"directory": "%s", "file": "Legacy/Source/B.cpp"}]'
                % (root, root),
                encoding="utf-8",
            )

            def undocumented_ast(header: Path, entries: list, component: Path) -> tuple:
                spelled = header.relative_to(component).as_posix()
                ast = [{
                    "kind": "NamespaceDecl", "loc": {"file": spelled, "line": 5},
                    "inner": [{"kind": "FunctionDecl", "name": "undocumented", "loc": {"line": 6}}],
                }]
                return ast, spelled, None

            with mock.patch.object(comments, "_ast_for_header", undocumented_ast):
                findings, _ = comments.compiler_public_api_findings(root, database)
            self.assertEqual(
                findings,
                ["Legacy/Include/rojoRHI/Device.h:6: public API declaration has no Doxygen comment"],
            )


if __name__ == "__main__":
    unittest.main()
