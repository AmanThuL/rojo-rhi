from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import check_cpp_layout as layout


def separator(indent: str = "") -> str:
    return indent + "//" + "=" * (118 - len(indent))


def file_header(name: str) -> list[str]:
    return [layout.FILE_RULER, f"/// @file {name}", "/// @brief Describes this file.", layout.FILE_RULER]


class CppLayoutTests(unittest.TestCase):
    def test_file_header_hyphen_envelope_is_not_a_decorative_ruler(self) -> None:
        lines = [*file_header("Empty.cpp"), "", "#include <metal>"]
        self.assertEqual(layout.check_layout(Path("Source/Empty.cpp"), "\n".join(lines), set()), [])

    def test_hyphen_ruler_outside_file_header_is_rejected(self) -> None:
        lines = [*file_header("Empty.cpp"), "", layout.FILE_RULER]
        errors = layout.check_layout(Path("Source/Empty.cpp"), "\n".join(lines), set())
        self.assertTrue(any("alternate" in error for error in errors))

    def test_function_and_rationale_comment_claim_one_separator(self) -> None:
        text = "\n".join([separator(), "// Explains a lifetime invariant.", "void draw() {}"])
        self.assertEqual(layout.check_layout(Path("Backends/Metal4/Source/Draw.cpp"), text, {2}), [])

    def test_indented_inline_definition_keeps_physical_width(self) -> None:
        text = "\n".join(["class Local {", "public:", separator("    "), "    void run() {}", "};"])
        self.assertEqual(layout.check_layout(Path("Tests/Local.cpp"), text, {3}), [])
        self.assertEqual(len(text.splitlines()[2]), 120)

    def test_template_prefix_claims_the_function_separator(self) -> None:
        text = "\n".join([separator(), "template <typename T>", "T load() { return {}; }"])
        self.assertEqual(layout.check_layout(Path("Backends/Metal4/Source/Load.cpp"), text, {2}), [])

    def test_missing_orphan_malformed_and_alternate_rulers_are_reported(self) -> None:
        source = "\n".join(
            [
                "void missing() {}",
                "",
                "//==========",
                "",
                separator(),
                "//---------- old section ----------",
            ]
        )
        errors = layout.check_layout(Path("Source/Broken.cpp"), source, {0})
        self.assertTrue(any("missing" in error for error in errors))
        self.assertTrue(any("alternate" in error for error in errors))
        self.assertTrue(any("orphan" in error for error in errors))

    def test_empty_translation_unit_is_valid(self) -> None:
        self.assertEqual(layout.check_layout(Path("Source/Empty.cpp"), "#include <metal>\n", set()), [])

    def test_catch_macro_symbols_at_one_line_are_deduplicated(self) -> None:
        uri = "file:///workspace/Tests/Example.cpp"
        symbols = [
            {
                "kind": 12,
                "name": "CATCH2_INTERNAL_TEST_0",
                "location": {"uri": uri, "range": {"start": {"line": 7}, "end": {"line": 7}}},
            },
            {
                "kind": 12,
                "name": "CATCH2_INTERNAL_TEST_0",
                "location": {"uri": uri, "range": {"start": {"line": 7}, "end": {"line": 9}}},
            },
            {"kind": 13, "location": {"uri": uri, "range": {"start": {"line": 9}}}},
        ]
        self.assertEqual(layout.definition_start_lines(symbols, uri, set()), {7})

    def test_plain_declaration_is_not_treated_as_a_definition(self) -> None:
        uri = "file:///workspace/Backends/Metal4/Source/Example.cpp"
        symbols = [
            {
                "kind": 12,
                "name": "declaredOnly",
                "location": {
                    "uri": uri,
                    "range": {"start": {"line": 3}, "end": {"line": 3}},
                },
            },
            {
                "kind": 12,
                "name": "defined",
                "location": {
                    "uri": uri,
                    "range": {"start": {"line": 5}, "end": {"line": 8}},
                },
            },
        ]
        self.assertEqual(layout.definition_start_lines(symbols, uri, {5}), {5})

    def test_semantic_definition_tokens_are_delta_decoded(self) -> None:
        # line delta, column delta, length, token type, modifier bitset
        data = [2, 4, 3, 1, 0, 3, 1, 5, 3, 2, 0, 8, 4, 3, 2]
        self.assertEqual(layout.semantic_definition_lines(data, definition_modifier=1), {5})

    def test_stale_compilation_database_has_actionable_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Source" / "New.cpp"
            source.parent.mkdir()
            source.write_text("void newFunction() {}\n", encoding="utf-8")
            database = root / "compile_commands.json"
            database.write_text(json.dumps([]), encoding="utf-8")
            with self.assertRaisesRegex(layout.LayoutSetupError, "rerun.*compile_commands"):
                layout.ensure_database_covers([source], database, root)

    def test_adapter_is_skipped_with_a_notice_when_the_database_lacks_it(self) -> None:
        root = Path("/component")
        validate = root / "Source/Validate.cpp"
        adapter = root / "Backends/Metal4/ImGui/Source/Metal4ImGui.cpp"
        files, notices = layout.select_checked_files([validate, adapter], {validate}, root)
        self.assertEqual(files, [validate])
        self.assertEqual(len(notices), 1)
        self.assertIn("Backends/Metal4/ImGui", notices[0])

    def test_adapter_is_checked_when_the_database_has_it(self) -> None:
        root = Path("/component")
        validate = root / "Source/Validate.cpp"
        adapter = root / "Backends/Metal4/ImGui/Source/Metal4ImGui.cpp"
        contract = root / "Backends/Metal4/ImGui/Source/ImGuiBackendContract.cpp"
        files, notices = layout.select_checked_files(
            [validate, adapter, contract], {validate, adapter}, root
        )
        self.assertEqual(files, [validate, adapter, contract])
        self.assertEqual(notices, [])

    def test_non_adapter_file_missing_from_the_database_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            covered = root / "Source" / "Validate.cpp"
            missing = root / "Backends" / "Metal4" / "Source" / "Metal4Device.cpp"
            for path in (covered, missing):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            database = root / "compile_commands.json"
            database.write_text(
                json.dumps([{"directory": str(root), "file": "Source/Validate.cpp"}]), encoding="utf-8"
            )
            files, _ = layout.select_checked_files(
                [covered, missing], layout.compilation_database_files(database), root
            )
            with self.assertRaisesRegex(layout.LayoutSetupError, "Metal4Device.cpp"):
                layout.ensure_database_covers(files, database, root)

    def test_an_empty_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            argv = ["check_cpp_layout.py", "--root", directory]
            with mock.patch.object(sys, "argv", argv), mock.patch("sys.stderr"):
                self.assertNotEqual(layout.main(), 0)


if __name__ == "__main__":
    unittest.main()
