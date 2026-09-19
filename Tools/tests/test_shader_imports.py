from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import check_shader_imports
from Tools.check_shader_imports import check_shaders


class ShaderImportTests(unittest.TestCase):
    def check(self, sources: dict[str, str]) -> tuple[list[str], int, int]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in sources.items():
                path = root / "Shaders" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
            return check_shaders(root)

    def test_oracles_and_modules_import_shared_modules(self) -> None:
        errors, files, imports = self.check({
            "Tests/ShadowSmoke.slang": "import Shadow; import Nested.Color;",
            "Tests/Oracle.slang": "import Shadow;",
            "Tests/Modules/Shadow.slang": "import Nested::Color;",
            "Tests/Modules/Nested/Color.slang": "float value;",
        })
        self.assertEqual(errors, [])
        self.assertEqual((files, imports), (4, 4))

    def test_oracle_cannot_import_a_module_the_component_lacks(self) -> None:
        errors, _, _ = self.check({"Tests/Smoke.slang": "import Lighting;"})
        self.assertTrue(any("unresolved module import 'Lighting'" in error for error in errors))

    def test_comments_and_strings_are_not_imports(self) -> None:
        errors, _, imports = self.check({
            "Tests/Oracle.slang": '// import Bad;\n/* import Bad; */\n"import Bad;";\nimport Color;',
            "Tests/Modules/Color.slang": "float value;",
        })
        self.assertEqual(errors, [])
        self.assertEqual(imports, 1)

    def test_multiline_imports_and_comments_preserve_diagnostic_lines(self) -> None:
        errors, _, imports = self.check({
            "Tests/Oracle.slang": "/* comment\ncomment */\nimport /* shared */\n Color\n;\nimport Missing;",
            "Tests/Modules/Color.slang": "",
        })
        self.assertEqual(imports, 2)
        self.assertEqual(len(errors), 1)
        self.assertIn("Shaders/Tests/Oracle.slang:6: unresolved", errors[0])

    def test_comment_and_string_entry_examples_do_not_make_module_an_entry(self) -> None:
        errors, _, _ = self.check({
            "Tests/Modules/Color.slang": '// [shader("compute")]\n"[shader( ";',
        })
        self.assertEqual(errors, [])

    def test_imports_never_reach_entries(self) -> None:
        for source in ("Tests/Modules/Shared.slang", "Tests/Other.slang"):
            for target in ("Tests/Oracle.slang", "Tests/Nested/Pass.slang"):
                with self.subTest(source=source, target=target):
                    errors, _, _ = self.check({source: f"import {Path(target).stem};", target: ""})
                    self.assertTrue(
                        any("imports may target Shaders/Tests/Modules only" in error for error in errors)
                    )

    def test_missing_and_wrong_case_modules_fail(self) -> None:
        for name in ("Missing", "color"):
            errors, _, _ = self.check({"Tests/Oracle.slang": f"import {name};", "Tests/Modules/Color.slang": ""})
            self.assertTrue(any("unresolved module" in error for error in errors))

    def test_duplicate_output_basenames_fail_including_case_collisions(self) -> None:
        for duplicate in ("Triangle", "triangle"):
            errors, _, _ = self.check({"Tests/Triangle.slang": "", f"Tests/Modules/{duplicate}.slang": ""})
            self.assertTrue(any("duplicate shader output basename" in error for error in errors))

    def test_modules_cannot_own_entry_points(self) -> None:
        errors, _, _ = self.check({"Tests/Modules/Shadow.slang": '[shader("compute")] void main() {}'})
        self.assertTrue(any("cannot declare shader entry points" in error for error in errors))

    def test_unsupported_and_incomplete_imports_fail(self) -> None:
        for declaration in ('import "../Tests/Oracle";', "import Color", "import ;"):
            errors, _, _ = self.check({"Tests/Oracle.slang": declaration})
            self.assertTrue(any("expected a named module import" in error for error in errors))

    def test_textual_include_cannot_bypass_module_boundary(self) -> None:
        errors, _, _ = self.check({"Tests/Smoke.slang": '#include "Tests/Oracle.slang"'})
        self.assertTrue(any("textual includes" in error for error in errors))

    def test_an_empty_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            argv = ["check_shader_imports.py", "--root", directory]
            with mock.patch.object(sys, "argv", argv), mock.patch("sys.stderr"):
                self.assertNotEqual(check_shader_imports.main(), 0)


if __name__ == "__main__":
    unittest.main()
