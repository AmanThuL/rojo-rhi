# Extracting RojoRHI from Luminex

**Status**: Accepted

R2.1 to R2.4 are the consumer's four steps towards the extraction, named here and in
`Tools/Rename/substitution.tsv` only: R2.1 writes these founding documents, R2.2 decouples the
component in place, R2.3 runs the mechanical rename below, and R2.4 extracts the component into
this repository and mounts it back as a submodule.

This guide explains the mechanical substitution that R2.3 applies to move Luminex's `RHI/`
component to the RojoRHI identity, and then the extraction R2.4 runs. The ordered rows live in
`Tools/Rename/substitution.tsv`; `Tools/Rename/apply.py` applies them to a Luminex checkout. This
guide records why each row exists, what the dry runs against Luminex found, what the rows
deliberately do not touch, and what still needs a hand edit.

## Column vocabulary

`substitution.tsv` has one header row and eight columns. The reader decodes the bytes as UTF-8
without newline translation, drops one trailing newline, splits rows on `\n` alone and fields on
the tab character, with no quoting: a `"` anywhere in a field, such as the anchors of rows 40, 41,
42, 70, 90 and 100, is ordinary field content. A default-dialect CSV reader would merge fields.

- `order` — an integer, strictly increasing; rows apply low to high.
- `kind` — `path` moves a tracked file or directory with `git mv`; `regex` rewrites file contents
  with Python's `re` module, so `\b`, `\t`, `\s` and lookbehind carry `re`'s meaning. Every `path`
  row precedes every `regex` row, and the reader rejects a table that breaks this order.
- `include` / `exclude` — whitespace-separated path globs over `git ls-files`, spelled in
  post-rename paths because the `path` rows run first: `**/` spans zero or more directories, `**`
  anything, `*` anything but a slash. A file is in scope when it matches an `include` glob and no
  `exclude` glob. `path` rows leave both empty.
- `pattern` / `replacement` — the regex and its replacement, or a `path` row's source and
  destination. A `|` inside a pattern is an alternation operator.
- `strict` — `yes` makes `--check` fail when the pattern still matches anywhere outside `docs/`
  and Markdown after a run, in scope or not; `no` checks the row's scope only.
- `note` — the rationale, in a few words.

`apply.py <root>` refuses a checkout with uncommitted changes, verifies before any move that each
`path` row's source is tracked and its destination does not exist, then moves and rewrites.
`--dry-run` prints the table below without writing; `--check` verifies an applied tree and
reads `git ls-files`, so it runs on a tree whose moves are still staged or committed — after a
`git reset` of an applied tree it reports every moved path as still tracked; `--selftest` applies
the table to a built-in fixture that pins every row's behaviour.

## Identity being introduced

The substitution moves Luminex's component to the identity fixed by the owner: namespace
`rojoRHI` and `rojoRHI::metal4`; public includes under `<rojoRHI/...>`; macros `ROJORHI_ASSERT`
and `ROJORHI_LOG_*`; default GPU label prefix `rojorhi.`; build targets `RojoRHI`,
`RojoRHIMetal4ImGui` and `RojoRHITests` (build directory `rojorhi-test`); Lua names
`rojorhi_slang2metallib`, `rojorhi_thirdparty` and `rojorhi_imgui_target`; probe define
`ROJORHI_IMGUI_BACKEND_SOURCE`; directory `RojoRHI/`. This mixed-case spelling is the one approved
exception to the project's lowercase namespace and include-root convention, kept because RHI is
already written as an initialism and the mixed case preserves the word boundary.

The first dry run, at R2.1, is kept for provenance in [its own record](rename-dry-run-r2.1.md).

## Dry-run counts (Luminex, after R2.2, 2026-09-19)

`apply.py <root> --dry-run` against Luminex `main` after R2.2, with the R2.3 plan opened. Each
row counts its matches while the rows before it have already run. The last column counts matches
in files outside the row's scope, skipping `docs/` and Markdown; every nonzero cell there is
explained under "Deliberate leaves".

| row | inside component: matches / files | outside: matches / files | out of scope, not prose: matches / files |
|---|---|---|---|
| 10 | 23 paths moved | n/a | n/a |
| 11 | 1 paths moved | n/a | n/a |
| 20 | 101 paths moved | n/a | n/a |
| 30 | 248 / 67 | 249 / 51 | 0 / 0 |
| 31 | 0 / 0 | 1921 / 162 | 5 / 3 |
| 40 | 116 / 44 | 53 / 49 | 14 / 1 |
| 41 | 0 / 0 | 21 / 2 | 0 / 0 |
| 42 | 1 / 1 | 0 / 0 | 0 / 0 |
| 43 | 0 / 0 | 28 / 3 | 1 / 1 |
| 45 | 2 / 1 | 107 / 20 | 5 / 4 |
| 46 | 0 / 0 | 6 / 2 | 0 / 0 |
| 47 | 0 / 0 | 3 / 3 | 0 / 0 |
| 50 | 148 / 13 | 0 / 0 | 303 / 54 |
| 60 | 24 / 8 | 0 / 0 | 94 / 19 |
| 61 | 2 / 2 | 0 / 0 | 0 / 0 |
| 70 | 274 / 23 | 0 / 0 | 1870 / 136 |
| 80 | 0 / 0 | 59 / 7 | 0 / 0 |
| 85 | 1 / 1 | 18 / 4 | 0 / 0 |
| 86 | 1 / 1 | 2 / 2 | 0 / 0 |
| 87 | 14 / 2 | 3 / 2 | 0 / 0 |
| 90 | 1 / 1 | 7 / 3 | 0 / 0 |
| 100 | 4 / 2 | 53 / 11 | 0 / 0 |
| 101 | 0 / 0 | 2 / 1 | 0 / 0 |
| 102 | 0 / 0 | 1 / 1 | 0 / 0 |

A trial application of this table to a scratch Luminex checkout passed `--check`, configured,
built, passed all four test groups with the Metal validation layer, both checkpoint-A checks, every
policy checker and both Python tool suites; both test binaries listed the same case names and tags
as before the rename. `RojoRHI/` copied out alone configured, built and passed its own suite, and
its test binary links only `RojoRHI`, Catch2 and system frameworks.

## Rows added or changed after R2.2

R2.2 moved tests, smoke shaders, tools and build files under the component and created names the
R2.1 table never saw. The rows are re-derived against that tree.

- **Row 11** moves the ImGui adapter's own public include root,
  `Backends/Metal4/ImGui/Include/RHI`, as row 10 moves the core one. Row 20 then moves the
  directory, so every later scope is spelled under `RojoRHI/`.
- **Row 30** now covers every tracked file outside prose and is strict: after R2.2 the namespace
  also appears as `lmx::rhi::base`, in the contract's `forbidUndefined` symbol prefix and in the
  tool tests, and nothing named `lmx::rhi` may survive.
- **Row 40** also reaches `RojoRHI/xmake/*.lua`, whose comment quotes the include form; the row is
  strict, so a quoted `#include "RHI/` left in any non-prose file fails `--check`.
- **Rows 41, 43 and 45** replace the R2.1 header-list and `"RHI/` rows; see the ordering rationale.
  Row 45 now covers the CI workflow, `.gitignore`, `Source/Asset/xmake.lua`, `xmake/*.lua` (format
  globs, setup patch path, shader rule), `Tests/GpuTestSupport.h`'s directory include,
  `Tools/check_checkpoint_a.py`, the GPU-debug tools, and the component's own `xmake/*.lua` and
  `Tools/**/*.py`.
- **Rows 46 and 47** follow Markdown link targets into the moved tree, so the project policy's
  broken-link check still passes. They rewrite only the path inside `](...)`; link text and prose
  keep the old words.
- **Row 61** renames the ImGui buffer probe's define. It has no leading `\b`: the probe's runner
  spells it `-DLMX_IMGUI_BACKEND_SOURCE` on the compiler command line, where `D` and `L` are both
  word characters, so `\bLMX_` would miss it and the probe would stop compiling.
- **Rows 85, 86 and 87** carry the approved build-name reach: the test target and its
  `RHITests/unit|gpu` groups, its build directory, and the Lua rule and host globals. All three are
  strict.
- **Row 100** keeps its narrow file list, now spelled as globs, extended to the component's
  `xmake.lua`, `xmake/*.lua` and `Tools/*.py`.
- **Rows 101 and 102** keep `Tools/tests/test_module_deps.py`'s target-closure cases true. Row 100
  renames the fixture's `"RHI"` target; row 101 renames it in the expected messages
  (`depends on RHI outside`), and row 102 swaps the two expected messages whose order changes,
  because the test compares against a sorted list and `Render` sorts before `RojoRHI`.

## Ordering rationale for rows 40–45

`RHI/` means two things in Luminex. As the public include root it becomes `rojoRHI/`; as the
component directory it becomes `RojoRHI/`. Tool files hold both, often in one string, so four rows
separate them and their order carries the decision.

1. **Row 40** rewrites `#include "RHI/...` directives in C++ and in the component's build comment
   to the angle form `#include <rojoRHI/...>`, which ADR 0001 and
   [naming.md](../conventions/naming.md) fix for a consumer. Its C++-only scope is deliberate:
   `test_module_deps.py` writes quoted include directives inside Python fixture strings, and the
   checker under test resolves a quoted spec next to the includer first, so those fixtures keep the
   quoted form and are rewritten by rows 41 and 45 instead.
2. **Row 41** rewrites a quoted or backticked string that is exactly an include spelling,
   `RHI/<Name>.h` or `RHI/Metal4/<Name>.h`, to `rojoRHI/...` with its delimiter unchanged: the
   contract's `asset` and `shell` header lists, the fixture includes and docstrings.
3. **Row 42** rewrites `check_rhi_headers.py`'s glob, which is anchored at the include directory.
4. **Row 43** rewrites `Include/RHI/`, the include root spelled as a path segment, in tool files.
5. **Row 45** then rewrites every remaining `RHI/` not preceded by a lower-case letter to
   `RojoRHI/`. The lookbehind skips everything rows 40–43 produced, because each of those
   spellings now reads `rojoRHI/`.

Running row 45 before rows 41–43 would capitalise include spellings to `RojoRHI/Format.h`, which
the case-insensitive file system accepts and ADR 0001 forbids. After the trial application,
`git grep -nE 'RojoRHI/[A-Za-z0-9]+\.h"|rojoRHI/(Include|Source|Tests|Backends)'` prints nothing,
and no string in `Tools/tests/*.py` mixes the two spellings. A wider search also caught a
backticked `RHI/RHI.h` in a docstring, which is why row 41 accepts backticks. Moving a directive
from the quoted to the angled include group can leave an include block out of the formatter's
order; the formatting commit after R2.3 settles that, so `xmake format --check` is not a gate for
the mechanical commit.

## What the post-R2.2 dry run answered

- **Row 70 (default labels).** Every `"lmx.` string under the component is the component's own:
  default labels (`lmx.{buffer,texture}.{,placed.}unnamed`, `lmx.heap.unnamed`,
  `lmx.sampler.unnamed`, `lmx.pipeline.unnamed`, `lmx.pass.unnamed`, `lmx.temporal.scaler`,
  `lmx.pass.temporal.scaler`), device-owned objects (`lmx.device.*`, `lmx.queue.drain`,
  `lmx.imgui.formatCarrier`) and labels its own tests author (`lmx.test.*`). No label the renderer
  authors appears there, so the row is not narrowed. Labels in Luminex's own `Tests/` keep `lmx.`.
- **Row 70 consumers.** `Tools/GpuDebug/uniformlib.py` parses frame-data page labels; row 80
  covers it and its tests. `xctracelib.py` (read by `profile.py`) keeps only encoders whose label
  contains `lmx.`; it would not see a `rojorhi.*` default, but Luminex labels every pass it
  declares, so no encoder carries a default today. `spike_inventory.py` is different: its `lmx.`
  needle hunts the labels the component's own spike capture test authors, and row 70 renames those
  to `rojorhi.test.spike*`. After the rename its label hits for that trace drop to zero, and the
  tool still exits 0, so no gate catches it. Both are listed as hand edits.
- **Row 80.** `RojoRHI/Tests/CaptureTests.cpp` spells its labels only in quoted form, which row 70
  rewrites, so row 80 keeps `Tests/Capture*.cpp` and `Tools/GpuDebug/**` and gains no
  component path.
- **Row 86.** `set_targetdir(".../rhi-test")` on the test target in `RojoRHI/xmake/targets.lua`
  produces the directory, and row 86 reaches it; the CI workflow and `check_checkpoint_a.py` are its
  two consumers.
- **Row 100.** Every in-scope `"RHI"` names the target (`target`, `add_deps`, the contract's
  target lists and fixtures), the xmake project (`set_project("RHI")` becomes
  `set_project("RojoRHI")`), the contract's component entry, or the path segment
  (`check_checkpoint_a.py`, `check_cpp_layout.py`, `check_cpp_comments.py`, fixtures). No
  non-prose `"RHI"` lies outside the row's scope. The contract unit id `"rhi"` is lower-case and
  stays.

## Deliberate leaves

The nonzero out-of-scope cells in the post-R2.2 table, and other forms left on purpose:

- **Row 31, 5 / 3** — four prose comments inside the component and a docstring in
  `Tools/GpuDebug/xctracelib.py` name `rhi::` symbols; they are hand edits.
- **Row 40, 14 / 1** — the quoted fixture includes in `Tools/tests/test_module_deps.py`, rewritten
  by rows 41 and 45 as the ordering rationale explains; nothing remains for the strict check.
- **Row 43, 1 / 1** and **row 45, 5 / 4** — path comments in `Benchmarks/FrameData/Runner.h`,
  `Benchmarks/FrameData/DeliverPerDrawData.h`, `RojoRHI/Include/rojoRHI/Texture.h` and
  `RojoRHI/Backends/Metal4/Source/Metal4FrameArena.h`; they are hand edits.
- **Rows 50 and 60, 303 / 54 and 94 / 19** — `LMX_ASSERT` and `LMX_LOG_*` in Luminex's Core and
  its callers, which keep Luminex's own macros.
- **Row 70, 1870 / 136** — renderer-, scene- and test-owned `lmx.` labels outside the component.
- **The `lmx` namespace outside the component.** Row 30 rewrites `lmx::rhi` and its nested
  namespaces, never bare `lmx::`; row 31's lookbehind leaves `sprhi::` and `std::rhi::` alone.
- **`"RHI"` outside row 100's files** — none remains outside prose; a future one that names
  neither the target, the project nor the directory stays out.
- **Every `LMX_*` environment variable.** The component reads none. `LMX_CAPTURE_PATH` appears in
  a capture-failure message and `LMX_MAX_FRAMES` in a comment; both stay, because the environment
  belongs to whoever launches the binary.
- **Names that keep "RHI" as a domain word.** File names such as `RHIResultTests.cpp`,
  `RhiGpuTestSupport.h`, `check_rhi_headers.py` and the `RHI.h` umbrella header; Catch2 tags such
  as `[rhi]` and every case name; Luminex's `Render/RhiLog`; the `[RHI]` stderr prefix; comments
  that say "the RHI component"; and the `lmx-*` scratch file names the capture tests and the probe
  create.
- **Luminex `docs/` and Markdown prose** — only link targets move (rows 46 and 47).
- **Lines past 100 columns.** The rename lengthens some lines beyond 100 columns in files
  `xmake format` does not cover: `.github/workflows/ci.yml`, `Source/App/xmake.lua`,
  `Tools/check_project_policy.py`, `Tools/check_checkpoint_a.py`,
  `Tools/tests/test_module_deps.py`, `RojoRHI/Tools/ImGuiBufferProbe/run.py`,
  `RojoRHI/xmake/shaders.lua` and `xmake.lua`. Luminex enforces no column limit on them; wrapping,
  if wanted, belongs to the hand-edit commit.
- **No compatibility alias.** A `namespace lmx::rhi = rojoRHI;` shim would keep stale references
  compiling; ADR 0001 fixes one spelling so that they fail loudly instead.

## Hand edits deferred past R2.3

Known before the rename runs, but not expressed as rows, because each needs judgement a
substitution should not attempt. They land in a separate commit after the mechanical and
formatting commits.

- `RojoRHI/Backends/Metal4/Source/Metal4Capture.cpp` — the capture-failure message names
  `LMX_CAPTURE_PATH`. The wording stays until the message's owner is known after extraction.
- `RojoRHI/Backends/Metal4/Source/Metal4Resources.h` (three comments) and
  `RojoRHI/Backends/Metal4/Source/Metal4CommandList.cpp` (one) name `rhi::Heap`,
  `rhi::bytesPerPixel`, `rhi::validateTextureView` and `rhi::CommandList::textureBarrier`; row 31
  does not reach the component, and they read as `rojoRHI::` once edited.
- Nine Luminex test files (`Tests/EngineAssetTests.cpp`, `Tests/EngineSceneTestSupport.h`,
  `Tests/GpuLightClusterTests.cpp`, `Tests/GpuLightDebugViewTests.cpp`,
  `Tests/GpuSceneTableTests.cpp`, `Tests/GpuVisibilityContributionTests.cpp`,
  `Tests/GpuVisibilityRailTests.cpp`, `Tests/GpuVisibilityTests.cpp`,
  `Tests/GpuVisibilityWorkTests.cpp`) keep `namespace rhi = rojoRHI;`, which nothing uses after row
  31. The line is valid and silent; deleting a line is not a substitution.
- `Tools/GpuDebug/xctracelib.py`'s module docstring names `rhi::RenderPassDesc`.
- `RojoRHI/Include/rojoRHI/Texture.h` and `RojoRHI/Backends/Metal4/Source/Metal4FrameArena.h` —
  comments naming `RHI/Validate.h` and `RHI/Metal4/Metal4FrameData.h`.
- `Benchmarks/FrameData/Runner.h` and `Benchmarks/FrameData/DeliverPerDrawData.h` — comments
  naming `RHI/Include/RHI/Metal4/Metal4FrameData.h` and the `RHI/Include` surface.
- `RojoRHI/xmake.lua` — its comment gives the standalone command as `xmake f -P RHI`.
- `RojoRHI/xmake/targets.lua` — the test target's comment says its tests "link the RHI target
  alone"; the target is `RojoRHI`.
- `RojoRHI/Tools/ImGuiBufferProbe/README.md` — its command runs
  `python3 RHI/Tools/ImGuiBufferProbe/run.py`, and it names "a bare copy of the `RHI` component".
- `Tools/GpuDebug/spike_inventory.py` — widen its `lmx.` needle to also match `rojorhi.`, so the
  component's spike-test labels are found again.
- `Tools/GpuDebug/xctracelib.py`'s `LMX_LABEL_MARKER` with `profile.py`'s description of it —
  decide whether it also accepts `rojorhi.`.
- Luminex's `AGENTS.md`, README and guides — commands and paths (`xmake -P RojoRHI`,
  `RojoRHITests`, `rojorhi-test`). Other Luminex milestone, decision and architecture prose waits
  for R2.4.

`Tools/tests/test_module_deps.py` is no longer a hand edit: rows 41, 43, 45, 100, 101 and 102
rewrite its fixtures consistently, and the Python tool suite passes on the renamed tree.

## R2.4: the extraction

`Tools/Extract/extract.sh <luminex-url-or-path> <out-dir>` clones Luminex's `main` with
`--no-local`, resets it to the tag `r2.4-pre-extraction-evidence`, drops the tag, and runs
`git filter-repo` over `Tools/Extract/paths.txt`, which strips the `RojoRHI/` and `RHI/` roots and
rewrites each `(#123)`, a Luminex number, to `(AmanThuL/Luminex#123)`. Luminex is never touched.

`Tools/Extract/verify_import.py --source <luminex> --tag r2.4-pre-extraction-evidence --import
<out-dir>` exits 0 only when the imported `main` tree is `<tag>:RojoRHI`; each mapped commit holds
exactly its original's listed files with a leading `RojoRHI/` or `RHI/` stripped, which catches a
file a collapsed move lost; no dropped commit changed a listed path; authorship survives; and no
rewritten message holds `(#`.

`paths.txt` names the three roots the component has lived under — `Source/RHI/` until M4.1, `RHI/`
after R2.2, `RojoRHI/` after R2.3 — and the files that were its own before those moves: twenty-one
test units under `Tests/`, twelve shaders under `Shaders/` and five files under `Tools/`. Each has
a non-empty `git log` at the tag, `git log --format= --name-only <tag> -- Source/RHI RHI RojoRHI`
prints nothing outside those roots; only `RojoRHI/` survives there, so the tip is `<tag>:RojoRHI`.

Left behind on purpose, each still Luminex's at the tag and each answered by a copy the component
owns: `Tests/GpuTestSupport.h`, against `RojoRHI/Tests/RhiGpuTestSupport.h`; the `Shaders/Tests/`
spelling of the six oracles both trees need — `BufferHazardSmoke`, `ComputeImageSmoke`,
`FullscreenSample`, `MrtSmoke`, `SamplerSmoke` and `ShadowSmoke`; `Shaders/Shadow.slang` with
`Shaders/Modules/Shadow.slang`, against `RojoRHI/Shaders/Tests/Modules/Shadow.slang`;
`Source/Core/*`, against `RojoRHI/Source/Base`; and `xmake/shaders.lua`, against its own copy.

If rule 2 fails at the R2.2 commit because a move such as `Tests/X` to `RHI/Tests/X` collapses both
spellings onto one path, drop `--path-rename RHI/:`, so the old root keeps its `RHI/` spelling in
history. That deviates from the layout this repository fixed, so it is reported, never applied.

## After R2.3: rebase and force-replace `main`

This guide and its substitution table are authored on the orphan `foundations` branch, which holds
none of the placeholder history this repository started with. R2.4 rebases `foundations` onto the
history imported from Luminex, then force-replaces `main` with the result once the owner confirms
the replacement and the archive tag over the prior `main` has been verified. That destructive step
happens exactly once.
