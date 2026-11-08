from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from Tools import check_project_policy as policy


# A detached `git gc --auto` can write into objects/ while the temporary repository is removed.
QUIET_GIT = ["-c", "gc.auto=0", "-c", "maintenance.auto=false", "-c", "gc.autoDetach=false"]


class ProjectPolicyTests(unittest.TestCase):
    def test_read_text_accepts_utf8_without_extension_allowlist(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scene.json").write_text('{"scene": "test"}', encoding="utf-8")
            with mock.patch.object(policy, "ROOT", root):
                self.assertEqual(policy.read_text(Path("scene.json")), '{"scene": "test"}')

    def test_read_text_skips_binary_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "capture.bin").write_bytes(b"capture\0payload")
            with mock.patch.object(policy, "ROOT", root):
                self.assertIsNone(policy.read_text(Path("capture.bin")))

    def test_spec_and_design_documents_need_a_status(self) -> None:
        for path in (Path("docs/spec/03-buffers.md"), Path("docs/design/x/README.md")):
            errors: list[str] = []
            with mock.patch.object(policy, "read_text", return_value="# Title\n"):
                policy.check_markdown([path], errors)
            self.assertIn("missing status field", errors[0])

    def test_identity_check_covers_repository_relative_paths(self) -> None:
        errors: list[str] = []
        path = Path("Tools/privatehandle-notes.md")
        with (
            mock.patch.object(policy, "author_identity_tokens", return_value={"privatehandle"}),
            mock.patch.object(policy, "read_text", return_value="safe content"),
        ):
            policy.check_identity([path], errors)
        self.assertEqual(len(errors), 1)
        self.assertIn("repository-relative path", errors[0])

    def test_identity_check_covers_text_regardless_of_suffix(self) -> None:
        errors: list[str] = []
        with (
            mock.patch.object(policy, "author_identity_tokens", return_value={"privatehandle"}),
            mock.patch.object(policy, "read_text", return_value='owner = "privatehandle"'),
        ):
            policy.check_identity([Path(".clang-format")], errors)
        self.assertEqual(len(errors), 1)
        self.assertIn("outside metadata", errors[0])

    def test_a_named_preregistration_note_takes_its_own_budget_over_the_docs_one(self) -> None:
        note = Path("docs/research/2026-10-18-metal4-overhead.md")
        budget = policy.PREREGISTRATION_BUDGETS[note.as_posix()]
        for lines, failures in ((budget, 0), (budget + 1, 1)):
            errors: list[str] = []
            with mock.patch.object(policy, "read_text", return_value="line\n" * lines):
                policy.check_line_budgets([note], errors)
            self.assertEqual(len(errors), failures)
        self.assertEqual(budget, 320)
        self.assertIn(f"{budget}-line budget", errors[0])

    def test_other_research_notes_keep_the_docs_budget(self) -> None:
        errors: list[str] = []
        with mock.patch.object(policy, "read_text", return_value="line\n" * 301):
            policy.check_line_budgets([Path("docs/research/2026-10-20-other.md")], errors)
        self.assertEqual(len(errors), 1)
        self.assertIn("300-line budget", errors[0])

    def test_markdown_check_rejects_unknown_status(self) -> None:
        path = Path("docs/specs/example.md")
        errors: list[str] = []
        with mock.patch.object(policy, "read_text", return_value="# Example\n\n**Status**: Maybe\n"):
            policy.check_markdown([path], errors)
        self.assertTrue(any("unsupported document status" in error for error in errors))

    def test_commit_policy_rejects_process_transcript_and_past_tense_subject(self) -> None:
        records = (
            "0123456789abcdef\0scene: added Sponza\n\nClose the backlog after subagent review.\0"
        )
        errors: list[str] = []
        with (
            mock.patch.object(policy, "git", return_value=records),
            mock.patch.object(policy, "author_identity_tokens", return_value=set()),
        ):
            policy.check_commit_messages("base..head", errors)
        self.assertTrue(any("imperative mood" in error for error in errors))
        self.assertTrue(any("implementation-history" in error for error in errors))

    def test_commit_policy_accepts_an_engineering_outcome(self) -> None:
        records = "0123456789abcdef\0render: establish the Metal baseline\0"
        errors: list[str] = []
        with (
            mock.patch.object(policy, "git", return_value=records),
            mock.patch.object(policy, "author_identity_tokens", return_value=set()),
        ):
            policy.check_commit_messages("base..head", errors)
        self.assertEqual(errors, [])

    def test_commit_policy_accepts_the_d3d12_scope(self) -> None:
        records = "0123456789abcdef\0d3d12: add the backend option\0"
        errors: list[str] = []
        with (
            mock.patch.object(policy, "git", return_value=records),
            mock.patch.object(policy, "author_identity_tokens", return_value=set()),
        ):
            policy.check_commit_messages("base..head", errors)
        self.assertEqual(errors, [])

    def test_commit_policy_accepts_a_squash_merged_spike(self) -> None:
        records = "0123456789abcdef\0spike: decide the RHI execution model (#11)\0"
        errors: list[str] = []
        with (
            mock.patch.object(policy, "git", return_value=records),
            mock.patch.object(policy, "author_identity_tokens", return_value=set()),
        ):
            policy.check_commit_messages("base..head", errors)
        self.assertEqual(errors, [])

    def test_public_copy_rejects_internal_milestones_and_unshipped_backends(self) -> None:
        errors: list[str] = []
        with mock.patch.object(
            policy,
            "read_text",
            return_value="M4 adds a Vulkan backend after the current renderer.",
        ):
            policy.check_public_copy([Path("README.md")], errors)
        self.assertEqual(len(errors), 2)

    def public_copy_errors(self, text: str) -> list[str]:
        errors: list[str] = []
        with mock.patch.object(policy, "read_text", return_value=text):
            policy.check_public_copy([Path("README.md")], errors)
        return errors

    def test_public_copy_accepts_d3d12_verified_on_warp_only(self) -> None:
        for name in ("Direct3D 12", "D3D12"):
            with self.subTest(name=name):
                text = f"{name} backend: implemented, verified on WARP only, not on hardware."
                self.assertEqual(self.public_copy_errors(text), [])

    def test_public_copy_rejects_unqualified_d3d12(self) -> None:
        self.assertEqual(len(self.public_copy_errors("A D3D12 backend is implemented.")), 1)

    def test_public_copy_rejects_d3d12_described_as_supported(self) -> None:
        text = "RojoRHI supports D3D12, verified on WARP only, not on hardware."
        self.assertEqual(len(self.public_copy_errors(text)), 1)

    def test_public_copy_rejects_the_phrase_in_another_sentence(self) -> None:
        text = "D3D12 is implemented. It is verified on WARP only, not on hardware."
        errors = self.public_copy_errors(text)
        self.assertEqual(len(errors), 1)
        self.assertIn("README.md:1:", errors[0])

    def test_public_copy_accepts_the_phrase_wrapped_across_lines(self) -> None:
        text = "D3D12 is implemented, verified on WARP\nonly,  not on\nhardware."
        self.assertEqual(self.public_copy_errors(text), [])

    def test_public_copy_rejects_unsupported_and_acceptance_beside_d3d12(self) -> None:
        for word in ("unsupported", "acceptance"):
            with self.subTest(word=word):
                text = f"D3D12 is {word}, verified on WARP only, not on hardware."
                self.assertEqual(len(self.public_copy_errors(text)), 1)

    def test_public_copy_always_rejects_vulkan(self) -> None:
        text = "Vulkan, verified on WARP only, not on hardware."
        self.assertEqual(len(self.public_copy_errors(text)), 1)

    def test_readme_passes_the_public_copy_check(self) -> None:
        errors: list[str] = []
        policy.check_public_copy([Path("README.md")], errors)
        self.assertEqual(errors, [])

    def test_public_copy_accepts_shipped_features_and_plain_future_themes(self) -> None:
        errors: list[str] = []
        with mock.patch.object(
            policy,
            "read_text",
            return_value="Metal 4 renderer. Next: HDR and physically based materials.",
        ):
            policy.check_public_copy([Path("README.md")], errors)
        self.assertEqual(errors, [])

    def test_process_policy_covers_extracted_build_scripts(self) -> None:
        for path in ("xmake.lua", "xmake/setup.lua", "xmake/targets.lua", "Source/Validate.cpp",
                     "Backends/Metal4/Source/Metal4Device.cpp", "Shaders/Tests/Triangle.slang",
                     "Adapters/ImGui/Source/ImGuiAdapter.cpp"):
            with self.subTest(path=path):
                errors: list[str] = []
                with mock.patch.object(policy, "read_text", return_value="-- Close the backlog."):
                    policy.check_process_narration([Path(path)], errors)
                self.assertEqual(len(errors), 1)

    def test_imgui_scope_is_accepted(self) -> None:
        self.assertIsNotNone(policy.COMMIT_SUBJECT.match("imgui: add a renderer"))

    def test_adapters_tree_is_checked_for_process_narration(self) -> None:
        errors: list[str] = []
        with mock.patch.object(policy, "read_text", return_value="// Ask the subagent."):
            policy.check_process_narration([Path("Adapters/ImGui/Source/ImGuiAdapter.cpp")], errors)
        self.assertEqual(len(errors), 1)

    def test_process_policy_covers_benchmark_sources(self) -> None:
        errors: list[str] = []
        with mock.patch.object(policy, "read_text", return_value="// Close the backlog."):
            policy.check_process_narration([Path("Benchmarks/Main.cpp")], errors)
        self.assertEqual(len(errors), 1)

    def test_fail_followed_by_a_statement_is_flagged(self) -> None:
        errors = policy.code_after_fail_errors("a.cpp", 'void f() {\n  FAIL("x");\n  return;\n}\n')
        self.assertEqual(len(errors), 1)
        self.assertIn("a.cpp:2:", errors[0])

    def test_multi_line_fail_with_nested_parens_is_flagged(self) -> None:
        text = 'int f() {\n  FAIL("a ) " << g(1,\n    h(2)));\n  return 0;\n}\n'
        self.assertEqual(len(policy.code_after_fail_errors("a.cpp", text)), 1)

    def test_fail_ending_its_block_is_accepted(self) -> None:
        text = 'void f() {\n  FAIL("x");  // done\n  /* note */\n}\n'
        self.assertEqual(policy.code_after_fail_errors("a.cpp", text), [])

    def test_fail_check_and_braceless_fail_are_accepted(self) -> None:
        text = 'int f() {\n  FAIL_CHECK("x");\n  if (a) FAIL("y");\n  return 1;\n}\n'
        self.assertEqual(policy.code_after_fail_errors("a.cpp", text), [])

    def test_fail_in_a_string_or_comment_is_ignored(self) -> None:
        text = 'void f() {\n  // FAIL("x"); g();\n  s = "FAIL(1); g();"; g();\n}\n'
        self.assertEqual(policy.code_after_fail_errors("a.cpp", text), [])

    def test_fail_scan_covers_v2_sources_only(self) -> None:
        text = 'void f() {\n  FAIL("x");\n  g();\n}\n'
        for name, expected in (("Tests/a.cpp", 1), ("Adapters/ImGui/a.cpp", 1), ("Tools/a.cpp", 0)):
            errors: list[str] = []
            with mock.patch.object(policy, "read_text", return_value=text):
                policy.check_code_after_fail([Path(name)], errors)
            self.assertEqual(len(errors), expected, name)

    def test_commit_messages_are_checked_only_when_a_range_is_given(self) -> None:
        errors: list[str] = []
        with mock.patch.object(policy, "git", side_effect=AssertionError("git must not run")):
            policy.check_commit_messages(None, errors)
        self.assertEqual(errors, [])
        with mock.patch.object(sys, "argv", ["check_project_policy.py"]):
            self.assertIsNone(policy.parse_args().commits)

    def test_an_empty_root_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", *QUIET_GIT, "init", "-q", str(root)], check=True)
            with (
                mock.patch.object(policy, "ROOT", root),
                mock.patch.object(sys, "argv", ["check_project_policy.py"]),
                mock.patch("sys.stderr"),
            ):
                self.assertNotEqual(policy.main(), 0)


if __name__ == "__main__":
    unittest.main()
