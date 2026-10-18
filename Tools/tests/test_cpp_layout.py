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

    def test_deeply_namespace_indented_definition_keeps_physical_width(self) -> None:
        indent = " " * 20
        text = "\n".join([separator(indent), indent + "void run() {}"])
        self.assertEqual(layout.check_layout(Path("Source/Nested.cpp"), text, {1}), [])

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
                layout.ensure_database_covers([source], [database], root)

    def test_adapter_is_skipped_with_a_notice_when_the_database_lacks_it(self) -> None:
        root = Path("/component")
        validate = root / "Source/Validate.cpp"
        adapter = root / layout.ADAPTER_DIR / "Source/Metal4ImGui.cpp"
        files, notices = layout.select_checked_files([validate, adapter], {validate}, root)
        self.assertEqual(files, [validate])
        self.assertEqual(len(notices), 1)
        self.assertIn(layout.ADAPTER_DIR, notices[0])

    def test_adapter_is_checked_when_the_database_has_it(self) -> None:
        root = Path("/component")
        validate = root / "Source/Validate.cpp"
        adapter = root / layout.ADAPTER_DIR / "Source/Metal4ImGui.cpp"
        contract = root / layout.ADAPTER_DIR / "Source/ImGuiBackendContract.cpp"
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
                layout.ensure_database_covers(files, [database], root)

    def write_database(self, root: Path, name: str, files: list[Path]) -> Path:
        database = root / name / "compile_commands.json"
        database.parent.mkdir(parents=True, exist_ok=True)
        entries = [{"directory": str(root), "file": str(path)} for path in files]
        database.write_text(json.dumps(entries), encoding="utf-8")
        return database

    def test_file_listed_only_by_the_second_database_is_assigned_to_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core, backend = root / "Source/Core.cpp", root / "Backends/Metal4/Source/Device.cpp"
            first = self.write_database(root, "null", [core])
            second = self.write_database(root, "metal4", [backend])
            assigned = layout.assign_databases([core, backend], [first, second])
            self.assertEqual(assigned[second], [backend])

    def test_file_listed_by_both_databases_is_assigned_to_the_first(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core = root / "Source/Core.cpp"
            first = self.write_database(root, "null", [core])
            second = self.write_database(root, "metal4", [core])
            assigned = layout.assign_databases([core], [first, second])
            self.assertEqual(assigned[first], [core])
            self.assertEqual(assigned[second], [])

    def test_union_of_two_databases_covers_every_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core, backend = root / "Source/Core.cpp", root / "Backends/Metal4/Source/Device.cpp"
            first = self.write_database(root, "null", [core])
            second = self.write_database(root, "metal4", [backend])
            layout.ensure_database_covers([core, backend], [first, second], root)

    def test_file_listed_by_neither_database_is_named(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core, lost = root / "Source/Core.cpp", root / "Tests/Lost.cpp"
            first = self.write_database(root, "null", [core])
            second = self.write_database(root, "metal4", [core])
            with self.assertRaisesRegex(layout.LayoutSetupError, "Lost.cpp.*per backend"):
                layout.ensure_database_covers([core, lost], [first, second], root)

    def test_adapter_is_skipped_only_when_no_database_lists_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core = root / "Source/Core.cpp"
            adapter = root / layout.ADAPTER_DIR / "Source/Metal4ImGui.cpp"
            first = self.write_database(root, "null", [core])
            second = self.write_database(root, "metal4", [adapter])
            union = layout.databases_files([first, second])
            self.assertEqual(layout.select_checked_files([core, adapter], union, root)[0], [core, adapter])
            union = layout.databases_files([first])
            self.assertEqual(layout.select_checked_files([core, adapter], union, root)[0], [core])

    def test_relative_entries_with_differing_directories_resolve_to_one_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core = root / "Source/Core.cpp"
            build_a, build_b = root / "a", root / "b"
            databases = []
            for build, relative in ((build_a, "../Source/Core.cpp"), (build_b, "../Source/Core.cpp")):
                database = build / "compile_commands.json"
                database.parent.mkdir(parents=True)
                database.write_text(
                    json.dumps([{"directory": str(build), "file": relative}]), encoding="utf-8"
                )
                databases.append(database)
            assigned = layout.assign_databases([core], databases)
            self.assertEqual(assigned[databases[0]], [core])
            self.assertEqual(assigned[databases[1]], [])

    def test_symlinked_root_resolves_to_the_real_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            real = Path(directory).resolve() / "real"
            real.mkdir()
            link = Path(directory).resolve() / "link"
            link.symlink_to(real)
            database = self.write_database(real, "null", [link / "Source/Core.cpp"])
            self.assertEqual(
                layout.compilation_database_files(database), {real / "Source/Core.cpp"}
            )

    def test_the_same_database_twice_assigns_every_file_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            core, other = root / "Source/Core.cpp", root / "Source/Other.cpp"
            database = self.write_database(root, "null", [core, other])
            assigned = layout.assign_databases([core, other], [database, database])
            self.assertEqual(assigned, {database: [core, other]})

    def test_a_missing_database_is_named(self) -> None:
        missing = Path("/nonexistent/metal4/compile_commands.json")
        with self.assertRaisesRegex(layout.LayoutSetupError, "metal4/compile_commands.json is missing"):
            layout.compilation_database_files(missing)

    def test_benchmark_sources_are_checked(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "Benchmarks" / "Host" / "HostClock.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("", encoding="utf-8")
            self.assertEqual(layout.project_cpp_files(root), [source])

    def test_an_empty_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            argv = ["check_cpp_layout.py", "--root", directory]
            with mock.patch.object(sys, "argv", argv), mock.patch("sys.stderr"):
                self.assertNotEqual(layout.main(), 0)


if __name__ == "__main__":
    unittest.main()
