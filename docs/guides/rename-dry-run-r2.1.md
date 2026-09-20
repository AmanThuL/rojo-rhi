# Rename dry run at R2.1

**Status**: Superseded by [the rename record](luminex-extraction.md)

This is the first dry run of the Luminex-to-RojoRHI substitution, measured on 2026-09-19 with the
six-column table of that time. It is retained for provenance only: the table it measured no longer
exists, and the post-R2.2 counts in [the rename record](luminex-extraction.md) supersede it. What
follows is the record as it was first written.

## Dry-run counts (Luminex, at R2.1, 2026-09-19)

Measured against Luminex `main` at the commit R2.1 started from, with the six-column table of that
time, whose `scope` column read `component`, `consumers` or `all`. Kept for provenance.

| row | pattern (regex rows) | inside component: matches / files | outside component: matches / files |
|---|---|---|---|
| 30 | `\blmx::rhi\b` | 90 / 44 | 382 / 67 (`Source Tests Tools Benchmarks xmake.lua`) |
| 31 | `(?<![\w:])rhi::` | 4 / 2 (prose comments only; the row is consumers-scoped and leaves them) | 1,916 / 161 (`Source Tests Benchmarks`, `*.h` `*.cpp` `*.mm`) |
| 40 | `(#[ \t]*include[ \t]+)"RHI/([^"]+)"` | 99 / 34 | 68 / 57 (`Source Tests Tools Benchmarks`) |
| 41 | `"RHI/(Format\.h\|TextureDesc\.h)"` | n/a (consumers-only row) | 4 / 1 (`Tools/module_contract.json`) |
| 42 | `"RHI/\*\*/\*\.h"` | n/a (consumers-only row) | 1 / 1 (`Tools/check_rhi_headers.py`) |
| 45 | `"RHI/` | n/a (consumers-only row) | 13 / 5, counted after row 41 has already run (`xmake.lua`, `xmake/tasks.lua`, `Tools/check_project_policy.py`, `Tools/check_cpp_comments.py`, `Tools/module_contract.json`) |
| 50 | `\bLMX_ASSERT\b` | 146 / 11 | n/a (component-only row) |
| 60 | `\bLMX_LOG_(TRACE\|DEBUG\|INFO\|WARN\|ERROR)\b` | 20 / 6 | n/a (component-only row) |
| 70 | `"lmx\.` | 26 / 9 | n/a (component-only row) |
| 80 | `lmx\.(device\.frameData\|imgui\.formatCarrier)` | n/a (consumers-only row) | 60 / 8 (`Tests/CaptureTests.cpp`, `Tests/CaptureSchemaTests.cpp`, `Tools/GpuDebug/**`) |
| 90 | `"RHIMetal4ImGui"` | 1 / 1 (`RHI/Backends/Metal4/ImGui/xmake.lua`) | 4 / 2 (`Source/App/xmake.lua`, `Tools/module_contract.json`) |
| 100 | `"RHI"` | 2 / 2 (`RHI/xmake.lua`, `RHI/Backends/Metal4/ImGui/xmake.lua`) | 40 / 11 (six remaining `xmake.lua` files and `Tools/module_contract.json`, 18/7, plus `Tools/check_cpp_layout.py` 1/1, `Tools/check_cpp_comments.py` 2/1, `Tools/check_rhi_headers.py` 1/1 and `Tools/tests/test_module_deps.py` 18/1) |
