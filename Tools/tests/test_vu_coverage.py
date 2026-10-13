import tempfile
import unittest
from pathlib import Path

from Tools import check_vu_coverage


def tree(spec: str, tests: str, pending: str) -> Path:
    root = Path(tempfile.mkdtemp())
    (root / "docs/spec").mkdir(parents=True)
    (root / "Tests").mkdir()
    (root / "Tools").mkdir()
    (root / "docs/spec/01.md").write_text(spec, encoding="utf-8")
    (root / "Tests/A.cpp").write_text(tests, encoding="utf-8")
    (root / "Tools/vu_pending.txt").write_text(pending, encoding="utf-8")
    return root


RULE = "- `VU-rojo-core-0001` A rule.\n"
DEATH = 'TEST_CASE("VU-rojo-core-0001 a case", "[contract][death]") { ROJORHI_EXPECT_DEATH("VU-rojo-core-0001", [] {}); }\n'


class VuCoverageTests(unittest.TestCase):
    def errors(self, spec: str, tests: str, pending: str) -> list[str]:
        return check_vu_coverage.check(tree(spec, tests, pending))[0]

    def test_death_test_covers_an_identifier(self):
        errors, summary = check_vu_coverage.check(tree(RULE, DEATH, ""))
        self.assertEqual([], errors)
        self.assertIn("1 rules: 1 covered, 0 pending", summary)

    def test_retired_rule_is_ignored(self):
        spec = "- `VU-rojo-core-0001` Retired in 0.2.0: old.\n- `VU-rojo-core-0002` B.\n"
        self.assertEqual([], self.errors(spec, "", "VU-rojo-core-0002 edges\n"))

    def test_uncovered_rule_fails(self):
        errors = self.errors(RULE, "", "")
        self.assertEqual(1, len(errors))
        self.assertIn("VU-rojo-core-0001", errors[0])

    def test_uncovered_rule_passes_when_pending(self):
        self.assertEqual([], self.errors(RULE, "", "VU-rojo-core-0001 resources\n"))

    def test_pending_rule_that_is_covered_fails(self):
        errors = self.errors(RULE, DEATH, "VU-rojo-core-0001 resources\n")
        self.assertIn("remove it from the pending list", errors[0])

    def test_pending_rule_absent_from_spec_fails(self):
        errors = self.errors(RULE, DEATH, "VU-rojo-core-0009 edges\n")
        self.assertIn("VU-rojo-core-0009", errors[0])

    def test_pending_line_needs_a_known_part(self):
        self.assertIn("unknown part", self.errors(RULE, "", "VU-rojo-core-0001 later\n")[0])
        self.assertIn("expected", self.errors(RULE, "", "VU-rojo-core-0001\n")[0])

    def test_every_part_is_accepted(self):
        for part in ("resources", "recording", "edges", "tracker"):
            self.assertEqual([], self.errors(RULE, "", f"VU-rojo-core-0001 {part}\n"))

    def test_test_without_death_tag_does_not_cover(self):
        tests = 'TEST_CASE("VU-rojo-core-0001 a case", "[contract]") {}\n'
        self.assertEqual(1, len(self.errors(RULE, tests, "")))

    def test_name_without_identifier_is_ignored(self):
        tests = 'TEST_CASE("the harness fails", "[death]") {}\n'
        self.assertEqual(1, len(self.errors(RULE, tests, "")))

    def test_escaped_quote_in_name_is_parsed(self):
        tests = 'TEST_CASE("VU-rojo-core-0001 a \\"quoted\\" case", "[death]") { ROJORHI_EXPECT_DEATH("VU-rojo-core-0001", [] {}); }\n'
        self.assertEqual([], self.errors(RULE, tests, ""))

    def test_cross_reference_is_not_a_definition(self):
        self.assertEqual([], self.errors(RULE + "See `VU-rojo-core-0002` and - `VU-rojo-core-0003`.\n", DEATH, ""))

    def test_unsorted_or_duplicate_pending_fails(self):
        spec = RULE + "- `VU-rojo-core-0002` B.\n"
        self.assertIn("out of order", self.errors(spec, "", "VU-rojo-core-0002 edges\nVU-rojo-core-0001 edges\n")[0])
        self.assertIn("twice", self.errors(RULE, "", "VU-rojo-core-0001 edges\nVU-rojo-core-0001 edges\n")[0])

    def test_foundation_is_no_longer_a_part(self):
        self.assertIn("unknown part", self.errors(RULE, "", "VU-rojo-core-0001 foundation\n")[0])

    def test_line_comment_does_not_cover(self):
        self.assertEqual(1, len(self.errors(RULE, "// " + DEATH, "")))

    def test_block_comment_does_not_cover(self):
        self.assertEqual(1, len(self.errors(RULE, "/*\n" + DEATH + "*/\n", "")))

    def test_comment_markers_inside_a_string_are_not_comments(self):
        tests = 'const char* url = "http://x /* y"; ' + DEATH
        self.assertEqual([], self.errors(RULE, tests, ""))

    def test_if_zero_block_does_not_cover(self):
        self.assertEqual(1, len(self.errors(RULE, "#if 0\n" + DEATH + "#endif\n", "")))

    def test_else_of_if_zero_covers(self):
        self.assertEqual([], self.errors(RULE, "#if 0\n#else\n" + DEATH + "#endif\n", ""))

    def test_nested_conditional_in_if_zero_stays_dead(self):
        tests = "#if 0\n#ifdef X\n#endif\n" + DEATH + "#endif\n"
        self.assertEqual(1, len(self.errors(RULE, tests, "")))

    def test_argument_must_match_the_name(self):
        tests = DEATH.replace('EXPECT_DEATH("VU-rojo-core-0001"', 'EXPECT_DEATH("VU-rojo-core-0002"')
        errors = self.errors(RULE, tests, "")
        self.assertTrue(any("VU-rojo-core-0002 but is named for VU-rojo-core-0001" in error for error in errors))
        self.assertTrue(any("VU-rojo-core-0001 a case" in error for error in errors))

    def test_death_case_without_expect_call_is_an_error(self):
        tests = 'TEST_CASE("VU-rojo-core-0001 a case", "[death]") {}\n'
        self.assertTrue(any("has no ROJORHI_EXPECT_DEATH" in error for error in self.errors(RULE, tests, "")))

    def test_comments_are_allowed(self):
        self.assertEqual([], self.errors(RULE, "", "# note\n\nVU-rojo-core-0001 edges\n"))


if __name__ == "__main__":
    unittest.main()
