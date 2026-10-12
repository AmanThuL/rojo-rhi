from __future__ import annotations
import unittest
from Tools import check_spec

HEAD = "# 03. Buffers\n\n**Status**: Proposed\n**API version**: 0.1.0\n\n"
ENTRY = ("### `{name}`\n```cpp\nvoid f();\n```\n#### Valid usage\n- `{vu}` Rule.\n#### Mapping\n"
         "| | Metal 4 | D3D12 | Vulkan |\n|---|---|---|---|\n| Cost | {cost} | native | native |\n")

def run(*chapters: str) -> list[str]:
    errors: list[str] = []
    check_spec.check_chapters([(f"c{i}.md", text) for i, text in enumerate(chapters)], errors)
    return errors

class CheckSpecTests(unittest.TestCase):
    def test_valid_entry_passes(self) -> None:
        self.assertEqual(run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="cheap: one load")), [])
    def test_missing_api_version_is_rejected(self) -> None:
        text = HEAD.replace("**API version**: 0.1.0\n", "") + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native")
        self.assertIn("missing API version", run(text)[0])
    def test_unknown_cost_class_is_rejected(self) -> None:
        self.assertIn("cost class", run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="free"))[0])
    def test_missing_mapping_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").split("#### Mapping")[0]
        self.assertIn("missing mapping", run(text)[0])
    def test_duplicate_valid_usage_id_is_rejected_across_chapters(self) -> None:
        errors = run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native"),
                     HEAD + ENTRY.format(name="b", vu="VU-rojo-buf-0001", cost="native"))
        self.assertIn("duplicate VU-rojo-buf-0001", errors[0])
    def test_duplicate_entry_name_is_rejected_across_chapters(self) -> None:
        errors = run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native"),
                     HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0002", cost="native"))
        self.assertIn("duplicate entry a", errors[0])
    def test_chapter_declaring_no_entries_passes(self) -> None:
        self.assertEqual(run(HEAD + "**Entries**: none\n"), [])
    def test_chapter_without_entries_is_rejected(self) -> None:
        self.assertIn("no entries", run(HEAD)[0])
    def test_malformed_valid_usage_area_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf2-0001", cost="native")
        self.assertIn("malformed valid usage line", run(text)[0])
    def test_malformed_valid_usage_number_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-1", cost="native")
        self.assertIn("malformed valid usage line", run(text)[0])
    def test_empty_valid_usage_section_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("- `VU-rojo-buf-0001` Rule.\n", "")
        self.assertIn("empty valid usage section", run(text)[0])
    def test_no_rules_marker_is_accepted(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("- `VU-rojo-buf-0001` Rule.", "- None.")
        self.assertEqual(run(text), [])
    def test_trailing_whitespace_on_valid_usage_heading_is_accepted(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("#### Valid usage\n", "#### Valid usage  \n")
        self.assertEqual(run(text), [])
    def test_trailing_whitespace_on_mapping_header_is_accepted(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("| | Metal 4 | D3D12 | Vulkan |\n", "| | Metal 4 | D3D12 | Vulkan |  \n")
        self.assertEqual(run(text), [])
    def test_valid_usage_id_outside_section_is_not_counted(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native") + "- `VU-rojo-buf-0001` cited in prose after the table.\n"
        self.assertEqual(run(text), [])
    def test_cost_class_with_suffix_is_rejected(self) -> None:
        self.assertIn("cost class", run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native-ish"))[0])
    def test_cost_class_with_space_note_is_accepted(self) -> None:
        self.assertEqual(run(HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native (one call)")), [])
    def test_missing_signature_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("```cpp\nvoid f();\n```\n", "")
        self.assertIn("missing signature block", run(text)[0])
    def test_missing_valid_usage_heading_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("#### Valid usage\n", "")
        self.assertIn("missing valid usage section", run(text)[0])
    def test_cost_row_with_too_few_cells_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("| native | native |\n", "| native |\n")
        self.assertIn("cost class", run(text)[0])
    def test_cost_row_with_too_many_cells_is_rejected(self) -> None:
        text = HEAD + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native").replace("| native | native |\n", "| native | native | native |\n")
        self.assertIn("cost class", run(text)[0])
    def test_chapter_declaring_no_entries_with_entries_is_rejected(self) -> None:
        text = HEAD + "**Entries**: none\n\n" + ENTRY.format(name="a", vu="VU-rojo-buf-0001", cost="native")
        self.assertIn("declares no entries", run(text)[0])
